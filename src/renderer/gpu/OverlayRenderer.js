// Overlay renderer - cursors, peak markers, f1/f2 highlight band.
// Drawn last in each panel's viewport (after data and grid), using the same
// panel-local coordinate system (0,0 = top-left, w×h = panel content area).
// Cursor labels are enqueued into TextRenderer and flushed by the caller.

import { _compile, _link, hexRGBA } from './TextRenderer.js';
import { T, PEAK_PALETTE } from '../lib/tokens.js';
import { ML, MR, MT, MB } from './GridRenderer.js';
import { binToT, freqToT } from '../lib/freq.js';
import { valueToY } from '../lib/numeric-safe.js';

const VERT = `#version 300 es
in vec2 a_pos;
uniform vec2 u_res;
void main() {
  vec2 ndc = a_pos / u_res * 2.0 - 1.0;
  ndc.y = -ndc.y;
  gl_Position = vec4(ndc, 0.0, 1.0);
}`;

const FRAG = `#version 300 es
precision mediump float;
uniform vec4 u_color;
out vec4 outColor;
void main() { outColor = u_color; }`;

// Peak marker vertex shader - instanced small circles via line-loop quads.
const PEAK_VERT = `#version 300 es
in float a_angle;      // 0..2π, one vertex per circle segment
in vec2  a_center;     // per-instance: circle center in panel-local px
uniform vec2 u_res;
uniform float u_radius;
void main() {
  vec2 pos = a_center + vec2(cos(a_angle), sin(a_angle)) * u_radius;
  vec2 ndc = pos / u_res * 2.0 - 1.0;
  ndc.y = -ndc.y;
  gl_Position = vec4(ndc, 0.0, 1.0);
}`;

const PEAK_SEGS = 12;
// Per-peak colors mirror PEAK_PALETTE so the circles on the spectrum match
// the Peaks sidebar's color swatches one-to-one. Pre-converted to RGBA
// arrays at module load so the per-frame loop does no allocation.
const C_PEAK = PEAK_PALETTE.map((hex) => hexRGBA(hex, 1.0));
// Harmonic-bar overlay - matches the orange used for the periodicity toggle in
// the control strip so the visual ties back to the button.
const C_HARM     = hexRGBA(T.orange, 0.30);
const C_HARM_LBL = hexRGBA(T.orange, 0.85);

export class OverlayRenderer {
  constructor(gl) {
    this.gl = gl;

    // Line program for cursors and highlight band borders
    this.lineProg = _link(gl,
      _compile(gl, gl.VERTEX_SHADER, VERT),
      _compile(gl, gl.FRAGMENT_SHADER, FRAG)
    );
    this.lineBuf = gl.createBuffer();
    this._lLine = {
      aPos:  gl.getAttribLocation(this.lineProg, 'a_pos'),
      uRes:  gl.getUniformLocation(this.lineProg, 'u_res'),
      uCol:  gl.getUniformLocation(this.lineProg, 'u_color'),
    };

    // Peak marker program (instanced circle segments)
    this.peakProg = _link(gl,
      _compile(gl, gl.VERTEX_SHADER, PEAK_VERT),
      _compile(gl, gl.FRAGMENT_SHADER, FRAG)
    );
    // Angle buffer for a circle
    const angles = new Float32Array(PEAK_SEGS + 1);
    for (let i = 0; i <= PEAK_SEGS; i++) angles[i] = (i / PEAK_SEGS) * Math.PI * 2;
    this.angleBuf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, this.angleBuf);
    gl.bufferData(gl.ARRAY_BUFFER, angles, gl.STATIC_DRAW);
    this.peakCenterBuf = gl.createBuffer();
    this._lPeak = {
      aAngle:  gl.getAttribLocation(this.peakProg, 'a_angle'),
      aCenter: gl.getAttribLocation(this.peakProg, 'a_center'),
      uRes:    gl.getUniformLocation(this.peakProg, 'u_res'),
      uRadius: gl.getUniformLocation(this.peakProg, 'u_radius'),
      uColor:  gl.getUniformLocation(this.peakProg, 'u_color'),
    };

    // Reusable scratch typed arrays for line/quad/peak uploads - avoids
    // `new Float32Array(...)` per draw call.
    this._lineScratch = new Float32Array(512);
    this._quadScratch = new Float32Array(8);
    this._peakScratch = new Float32Array(32); // grows on demand
    this.dpr = 1;
  }

  // Cursor lines + label + highlight band live in React (CursorOverlay
  // in RenderSurface.jsx). They update only when the user clicks, so a
  // per-frame GPU pass was wasted work.

  // Draw peak markers (circles) and enqueue labels into TextRenderer.
  // centerFreq + twoSided are honoured so the kHz/MHz/GHz label reads as the
  // absolute frequency at the peak bin (matches the grid axis).
  // Draw colored circle markers at each detected peak. No text labels -
  // the Peaks sidebar tab shows the freq/dB/prominence info textually
  // and color-matches each row to the circle on the spectrum.
  // `text` parameter kept for call-site compatibility; ignored here.
  drawPeaks(text, w, h, sampleRate, peaks,
            centerFreq = 0, twoSided = false, scale = 'linear',
            xMin = 0, xMax = 1, dBmin = -130, dBmax = 10) {
    if (!peaks || peaks.length === 0) return;
    const d = this.dpr;
    const ml = ML * d, mr = MR * d, mt = MT * d, mb = MB * d;
    const plotH = h - mb - mt;
    const plotW = w - ml - mr;
    const span = xMax - xMin;

    // Each peak gets its own color from PEAK_PALETTE, so we issue one
    // draw call per peak (uploading a single center each time). With
    // max_peaks=8 this is cheap; using per-instance color attributes
    // would be marginally faster but adds buffer plumbing for no real
    // gain at this scale.
    const gl = this.gl;
    const centers = this._peakScratch; // reused across calls

    gl.useProgram(this.peakProg);
    const l = this._lPeak;
    gl.uniform2f(l.uRes, w, h);
    gl.uniform1f(l.uRadius, 4.0 * d);

    gl.bindBuffer(gl.ARRAY_BUFFER, this.angleBuf);
    gl.enableVertexAttribArray(l.aAngle);
    gl.vertexAttribPointer(l.aAngle, 1, gl.FLOAT, false, 0, 0);
    gl.vertexAttribDivisor(l.aAngle, 0);

    for (let i = 0; i < peaks.length; i++) {
      const { binIdx, value, N } = peaks[i];
      // Use the shader-aligned bin->t convention so the marker lands on the
      // same pixel column as the spectrum line's peak for this bin. See
      // binToT() in lib/freq.js for why this is NOT (bin + 0.5)/N.
      const t = binToT(binIdx, N);
      const f = twoSided
        ? centerFreq + (t - 0.5) * sampleRate
        : t * (sampleRate / 2);
      const tAxis = freqToT(f, sampleRate, centerFreq, twoSided, scale);
      const tView = (tAxis - xMin) / span;
      // Skip peaks that fall outside the visible viewport - drawing them
      // clamped to the edge would lie about where the peak actually is.
      if (tView < 0 || tView > 1) continue;
      const px = ml + tView * plotW;
      const py = valueToY(value, dBmin, dBmax, mt, plotH);
      // valueToY returns null when dBmax <= dBmin (degenerate auto-range)
      // or the peak's value is non-finite. Skip the marker rather than
      // clamping a NaN to the canvas corner.
      if (py == null) continue;
      const cpx = Math.max(ml + 3 * d, Math.min(w - mr - 3 * d, px));
      const cpy = Math.max(mt + 3 * d, Math.min(h - mb - 3 * d, py));
      centers[0] = cpx; centers[1] = cpy;

      gl.bindBuffer(gl.ARRAY_BUFFER, this.peakCenterBuf);
      gl.bufferData(gl.ARRAY_BUFFER, centers.subarray(0, 2), gl.DYNAMIC_DRAW);
      gl.enableVertexAttribArray(l.aCenter);
      gl.vertexAttribPointer(l.aCenter, 2, gl.FLOAT, false, 0, 0);
      gl.vertexAttribDivisor(l.aCenter, 1);

      gl.uniform4fv(l.uColor, C_PEAK[i % C_PEAK.length]);
      gl.drawArraysInstanced(gl.LINE_STRIP, 0, PEAK_SEGS + 1, 1);
    }
  }

  // Draw harmonic bars - vertical lines at f, 2f, 3f, … of the detected
  // fundamental, with small ×N labels at the top of each bar. Only the
  // harmonics that fall inside the plot area are drawn. Lines are batched
  // into a single GL_LINES upload.
  drawHarmonics(text, w, h, sampleRate, fundamental,
                centerFreq = 0, twoSided = false, scale = 'linear',
                xMin = 0, xMax = 1, maxN = 12) {
    if (!fundamental || fundamental <= 0 || !isFinite(fundamental)) return;
    const d = this.dpr;
    const x0 = ML * d, x1 = w - MR * d;
    const yT = MT * d, yB = h - MB * d;
    const xRange = x1 - x0;
    const span = xMax - xMin;

    const need = maxN * 4;
    if (this._lineScratch.length < need) {
      this._lineScratch = new Float32Array(need * 2);
    }
    const verts = this._lineScratch;
    let count = 0;
    // Stash (n, x) inline so we can also enqueue labels in the same pass -
    // avoids running freqToT twice per harmonic.
    const labels = []; // small (≤ maxN) - fine to allocate per frame
    for (let n = 1; n <= maxN; n++) {
      const f = fundamental * n;
      const tAxis = freqToT(f, sampleRate, centerFreq, twoSided, scale);
      const tView = (tAxis - xMin) / span;
      if (tView < 0 || tView > 1) continue;
      const x = x0 + tView * xRange;
      verts[count]   = x; verts[count+1] = yT;
      verts[count+2] = x; verts[count+3] = yB;
      count += 4;
      labels.push({ n, x });
    }
    if (count === 0) return;

    this._drawScratchLines(count, w, h, C_HARM);

    // ×N label near the top of each bar - small so it doesn't fight the
    // grid axis labels at the top edge.
    const SC = 0.625;
    for (const { n, x } of labels) {
      const lbl = '×' + n;
      text.enqueue(lbl, x + 2 * d, yT + 2 * d, C_HARM_LBL, SC);
    }
  }

  // Private helpers
  // Filled rectangle - used by GPURenderer to paint the dark strip
  // behind the spectrogram's React-rendered ruler labels. Cursor-band
  // rendering moved to React (CursorOverlay), so the ruler strip is
  // the only remaining caller.
  _drawQuad(x0, y0, x1, y1, w, h, color) {
    const gl = this.gl;
    gl.useProgram(this.lineProg);
    const l = this._lLine;
    gl.uniform2f(l.uRes, w, h);
    gl.uniform4fv(l.uCol, color);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.lineBuf);
    const q = this._quadScratch;
    q[0] = x0; q[1] = y0;
    q[2] = x1; q[3] = y0;
    q[4] = x1; q[5] = y1;
    q[6] = x0; q[7] = y1;
    gl.bufferData(gl.ARRAY_BUFFER, q, gl.DYNAMIC_DRAW);
    gl.enableVertexAttribArray(l.aPos);
    gl.vertexAttribPointer(l.aPos, 2, gl.FLOAT, false, 0, 0);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
    gl.drawArrays(gl.TRIANGLE_FAN, 0, 4);
    gl.disable(gl.BLEND);
  }

  // Upload `count` floats from the line scratch buffer and draw as GL_LINES.
  _drawScratchLines(count, w, h, color) {
    if (count < 4) return;
    const gl = this.gl;
    gl.useProgram(this.lineProg);
    const l = this._lLine;
    gl.uniform2f(l.uRes, w, h);
    gl.uniform4fv(l.uCol, color);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.lineBuf);
    gl.bufferData(gl.ARRAY_BUFFER,
      this._lineScratch.subarray(0, count), gl.DYNAMIC_DRAW);
    gl.enableVertexAttribArray(l.aPos);
    gl.vertexAttribPointer(l.aPos, 2, gl.FLOAT, false, 0, 0);
    gl.drawArrays(gl.LINES, 0, count / 2);
  }

  destroy() {
    const gl = this.gl;
    if (this.lineProg)       gl.deleteProgram(this.lineProg);
    if (this.peakProg)       gl.deleteProgram(this.peakProg);
    if (this.lineBuf)        gl.deleteBuffer(this.lineBuf);
    if (this.angleBuf)       gl.deleteBuffer(this.angleBuf);
    if (this.peakCenterBuf)  gl.deleteBuffer(this.peakCenterBuf);
  }
}
