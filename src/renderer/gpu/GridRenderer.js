// Shared grid / axis renderer used by all analytical panels.
// Draws horizontal and vertical guide lines, then enqueues axis labels
// into the TextRenderer. Coordinates are viewport-local pixels.

import { _compile, _link, hexRGBA } from './TextRenderer.js';
import { T } from '../lib/tokens.js';
import { tToFreq, freqToT, fmtFreqShort, LOG_FMIN_HZ } from '../lib/freq.js';

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
void main() {
  outColor = u_color;
}`;

// Margin constants matching the old Canvas2D views.
export const ML = 34; // left  (dB labels)
export const MR = 8;  // right
export const MT = 12; // top
export const MB = 16; // bottom (freq labels)

const C_GRID  = hexRGBA(T.borderSub);
const C_LABEL = hexRGBA(T.text, 1.0); // on-dark cream - readable axis labels
const C_IQ_LABEL = hexRGBA(T.text, 0.85);
// DESIGN.md caption-md = 14px -> scale 14/16 = 0.875 (CELL=16 baseline at scale 1)
const C_LABEL_S = 0.875;

export class GridRenderer {
  constructor(gl, text) {
    this.gl = gl;
    this.text = text;
    this.program = _link(gl,
      _compile(gl, gl.VERTEX_SHADER, VERT),
      _compile(gl, gl.FRAGMENT_SHADER, FRAG)
    );
    this.buf = gl.createBuffer();
    this._pos = gl.getAttribLocation(this.program, 'a_pos');
    this._uRes = gl.getUniformLocation(this.program, 'u_res');
    this._uCol = gl.getUniformLocation(this.program, 'u_color');
    // Reusable scratch typed array for line vertices - grows on demand,
    // never per-frame allocations.
    this._lines = new Float32Array(2048);
    this._lineCount = 0;
    // DPR scale - set externally by GPURenderer.setDpr(). Margins/positions are
    // declared in CSS pixels and scaled by this when computing physical px.
    this.dpr = 1;
  }

  // Reset the per-call scratch buffer, then push 2D segments into it.
  _resetLines() { this._lineCount = 0; }
  _pushLine(x0, y0, x1, y1) {
    let cap = this._lines.length;
    if (this._lineCount + 4 > cap) {
      const next = new Float32Array(cap * 2);
      next.set(this._lines);
      this._lines = next;
    }
    const o = this._lineCount;
    this._lines[o]   = x0; this._lines[o+1] = y0;
    this._lines[o+2] = x1; this._lines[o+3] = y1;
    this._lineCount += 4;
  }

  // Draw grid for a panel. w,h = viewport pixel size. viewType = 'spectrum'|'waveform'|'phase'|'histogram'
  // centerFreq + twoSided are only consulted for the spectrum + phase x-axis labels.
  // dBmin/dBmax = y-axis range for the spectrum grid lines + labels (Flatten
  // mode replaces the historical -130/10 defaults with EMA-smoothed
  // spectrum extrema).
  draw(w, h, sampleRate, viewType, centerFreq = 0, twoSided = false,
       scale = 'linear', xMin = 0, xMax = 1, dBmin = -130, dBmax = 10) {
    switch (viewType) {
      case 'spectrum':   this._spectrum(w, h, sampleRate, centerFreq, twoSided, scale, xMin, xMax, dBmin, dBmax); break;
      case 'waveform':   this._waveform(w, h, sampleRate);                                          break;
      case 'phase':      this._phase(w, h, sampleRate, centerFreq, twoSided, scale, xMin, xMax);    break;
      case 'histogram':  this._histogram(w, h);                                                     break;
    }
  }

// Spectrum: dB vert, frequency horiz
_spectrum(w, h, sr, centerFreq, twoSided, scale, xMin, xMax, dBmin, dBmax) {
    const d = this.dpr;
    const ml = ML * d, mr = MR * d, mt = MT * d, mb = MB * d;
    const plotH = h - mb - mt;
    const plotW = w - ml - mr;
    this._resetLines();

    // Pick a sensible tick step from the visible range so we always show
    // a few horizontal grid lines regardless of how zoomed-in the y-axis
    // is. Tick steps are powers of 2/5 of 10 dB so the labels stay
    // readable.
    const range = dBmax - dBmin;
    const targetTicks = 8;
    const rawStep = range / targetTicks;
    const pow10 = Math.pow(10, Math.floor(Math.log10(rawStep)));
    const norm = rawStep / pow10;
    const step = (norm >= 5 ? 5 : norm >= 2 ? 2 : 1) * pow10;
    const start = Math.ceil(dBmin / step) * step;
    for (let db = start; db <= dBmax; db += step) {
      const y = mt + ((dBmax - db) / range) * plotH;
      this._pushLine(ml, y, w - mr, y);
      const label = (Math.abs(db) < 1e-6 ? '0' : db.toFixed(step < 1 ? 1 : 0));
      this.text.enqueue(label, ml - 2 * d - this.text.measure(label, C_LABEL_S), y - 6 * d, C_LABEL, C_LABEL_S);
    }
    this._freqTicks(ml, mt, plotW, plotH, h - mb + 1 * d,
                    sr, centerFreq, twoSided, scale, /*labels*/ true,
                    xMin, xMax);

    this._flushLines(w, h, C_GRID, 0.5);
  }

  // Push freq-axis tick lines + (optionally) labels into the scratch buffer.
  // Linear: 9 evenly-spaced ticks across the visible viewport.
  // Log: decade majors that fall inside the viewport (plus unlabelled
  // 2..9× minors). `labelY` is the y-pixel where labels go; pass null/
  // undefined to skip them. xMin/xMax are axis-space [0,1] viewport bounds.
  _freqTicks(ml, mt, plotW, plotH, labelY, sr, centerFreq, twoSided, scale,
             labels, xMin = 0, xMax = 1) {
    const span = xMax - xMin;
    // Map an axis-space t to its on-screen viewport position.
    const screenT = (tAxis) => (tAxis - xMin) / span;
    // Anchor a label so it doesn't hang off the plot edge: leftmost tick
    // gets left-anchored, rightmost tick gets right-anchored, everything
    // in between stays centered. Same trick we use on the spectrogram
    // ruler - without it, the first/last labels lose half their width
    // to the panel scissor.
    const xRight = ml + plotW;
    const labelX = (tickX, measure) => {
      const halfText = measure * 0.5;
      if (tickX - halfText < ml)     return ml;                  // left edge
      if (tickX + halfText > xRight) return xRight - measure;    // right edge
      return tickX - halfText;
    };

    if (scale === 'log' && !twoSided) {
      const fmin = LOG_FMIN_HZ;
      const fmax = sr / 2;
      // Major ticks at decades. We still iterate the whole [fmin, fmax]
      // band rather than computing the visible window explicitly; the
      // (t < 0 || t > 1) check below filters to the viewport.
      const decStart = Math.pow(10, Math.floor(Math.log10(fmin)));
      for (let f = decStart; f <= fmax; f *= 10) {
        if (f < fmin) continue;
        const tAxis = freqToT(f, sr, centerFreq, twoSided, 'log');
        const tView = screenT(tAxis);
        if (tView < 0 || tView > 1) continue;
        const x = ml + tView * plotW;
        this._pushLine(x, mt, x, mt + plotH);
        if (labels) {
          const lbl = fmtFreqShort(f);
          const m = this.text.measure(lbl, C_LABEL_S);
          this.text.enqueue(lbl, labelX(x, m), labelY, C_LABEL, C_LABEL_S);
        }
      }
      // Minor ticks (2..9× each decade) - drawn but not labelled.
      for (let dec = decStart; dec <= fmax; dec *= 10) {
        for (let m = 2; m < 10; m++) {
          const f = dec * m;
          if (f < fmin || f > fmax) continue;
          const tAxis = freqToT(f, sr, centerFreq, twoSided, 'log');
          const tView = screenT(tAxis);
          if (tView <= 0 || tView >= 1) continue;
          const x = ml + tView * plotW;
          this._pushLine(x, mt, x, mt + plotH);
        }
      }
    } else {
      // 9 evenly-spaced ticks across the viewport. Each tick's label is
      // the absolute Hz at that on-screen position - computed by mapping
      // the screen fraction back through the viewport into axis-space.
      for (let i = 0; i <= 8; i++) {
        const tView = i / 8;
        const tAxis = xMin + tView * span;
        const x = ml + tView * plotW;
        this._pushLine(x, mt, x, mt + plotH);
        if (labels) {
          const f = tToFreq(tAxis, sr, centerFreq, twoSided, 'linear');
          const lbl = fmtFreqShort(f);
          const m = this.text.measure(lbl, C_LABEL_S);
          this.text.enqueue(lbl, labelX(x, m), labelY, C_LABEL, C_LABEL_S);
        }
      }
    }
  }

// Waveform: amplitude horiz, time vert
_waveform(w, h, sr) {
    const d = this.dpr;
    const ml = ML * d, mr = MR * d;
    const plotL = ml, plotR = w - mr;
    this._resetLines();
    // Horizontal amplitude lines at ±0.25, ±0.5, ±0.75, 0, ±1. Spans only
    // the plot rectangle so the labels on the left can live in the
    // reserved margin without the trace drawing over them.
    for (const amp of [1, 0.75, 0.5, 0.25, 0, -0.25, -0.5, -0.75, -1]) {
      const y = h / 2 - amp * (h / 2.2);
      this._pushLine(plotL, y, plotR, y);
      if (amp !== 0) {
        const lbl = amp.toFixed(2);
        // amp=±1 lines sit just outside the panel (h/2 ± h/2.2 ->
        // ~−0.045·h and ~1.045·h); pin the label y into the visible
        // region so neither end clips against the panel edge.
        const naturalY = y - 12 * d;
        const labelY = Math.max(2 * d, Math.min(h - 14 * d, naturalY));
        // Right-anchor the label in the left margin so it doesn't run
        // into the gridline. Same pattern as the spectrum dB labels.
        this.text.enqueue(lbl, ml - 2 * d - this.text.measure(lbl, C_LABEL_S),
                          labelY, C_LABEL, C_LABEL_S);
      }
    }
    // Vertical time gridlines, also clamped to the plot rectangle.
    for (let i = 0; i <= 10; i++) {
      const x = plotL + (i / 10) * (plotR - plotL);
      this._pushLine(x, 0, x, h);
    }
    this._flushLines(w, h, C_GRID, 0.5);
  }

// Phase: radians vert, frequency horiz
_phase(w, h, sr, centerFreq, twoSided, scale, xMin, xMax) {
    const d = this.dpr;
    const ml = ML * d, mr = MR * d, mt = MT * d, mb = MB * d;
    const plotH = h - mt - mb;
    const labels = ['π', 'π/2', '0', '-π/2', '-π'];
    this._resetLines();
    // Gridlines + radian labels span the plot area, leaving the top/bottom
    // margins free for the radian labels (top) and freq tick labels (bottom).
    // Without these margins the phase trace, which uses the full plot
    // height, draws over the labels.
    for (let i = 0; i < 5; i++) {
      const y = mt + (i / 4) * plotH;
      this._pushLine(ml, y, w - mr, y);
      const lbl = labels[i];
      this.text.enqueue(lbl, ml - 2 * d - this.text.measure(lbl, C_LABEL_S),
                        y - 6 * d, C_LABEL, C_LABEL_S);
    }
    this._freqTicks(ml, mt, w - ml - mr, plotH, h - mb + 1 * d,
                    sr, centerFreq, twoSided, scale, /*labels*/ true,
                    xMin, xMax);
    this._flushLines(w, h, C_GRID, 0.5);
  }

// Histogram
_histogram(w, h) {
    const d = this.dpr;
    const ml = ML * d, mr = MR * d, mb = MB * d;
    this._resetLines();
    for (let i = 0; i <= 4; i++) {
      const y = (i / 4) * (h - mb);
      this._pushLine(ml, y, w - mr, y);
      const pct = `${100 - i * 25}%`;
      // i=0 is the top "100%" line at y=0; default y - 6*d would clip
      // above the panel, so drop the label just below the line in that
      // case.
      const labelY = (i === 0) ? y + 2 * d : y - 6 * d;
      this.text.enqueue(pct, ml - 2 * d - this.text.measure(pct, C_LABEL_S),
                        labelY, C_LABEL, C_LABEL_S);
    }
    const baselineY = h - 14 * d;
    this.text.enqueue('-1.0', ml, baselineY, C_LABEL, C_LABEL_S);
    const mid = '0.0';
    this.text.enqueue(mid, ml + (w - ml - mr) / 2 - this.text.measure(mid, C_LABEL_S) / 2, baselineY, C_LABEL, C_LABEL_S);
    this.text.enqueue('+1.0', w - mr - this.text.measure('+1.0', C_LABEL_S), baselineY, C_LABEL, C_LABEL_S);
    this._flushLines(w, h, C_GRID, 0.5);
  }

  // IQ reference circles
  // Draws concentric circles and cross lines for IQ constellation.
  drawIQGrid(w, h) {
    const d = this.dpr;
    const cx = w / 2, cy = h / 2;
    const sc = Math.min(w, h) / 2.5;
    const SEGS = 64;
    this._resetLines();

    for (const r of [0.25, 0.5, 0.75, 1.0]) {
      for (let i = 0; i < SEGS; i++) {
        const a0 = (i / SEGS) * Math.PI * 2;
        const a1 = ((i + 1) / SEGS) * Math.PI * 2;
        this._pushLine(cx + Math.cos(a0) * r * sc, cy + Math.sin(a0) * r * sc,
                       cx + Math.cos(a1) * r * sc, cy + Math.sin(a1) * r * sc);
      }
    }
    this._pushLine(0, cy, w, cy);
    this._pushLine(cx, 0, cx, h);

    this._flushLines(w, h, C_GRID, 0.5);

    this.text.enqueue('I', w - 14 * d, cy - 12 * d, C_IQ_LABEL, C_LABEL_S);
    this.text.enqueue('Q', cx + 4 * d, 2 * d, C_IQ_LABEL, C_LABEL_S);
  }

  // Upload the scratch buffer (filled by _pushLine) and draw it.
  _flushLines(w, h, color, lineWidth = 1.0) {
    if (this._lineCount < 4) return;
    const gl = this.gl;
    gl.useProgram(this.program);
    gl.uniform2f(this._uRes, w, h);
    gl.uniform4fv(this._uCol, color);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.buf);
    gl.bufferData(gl.ARRAY_BUFFER,
      this._lines.subarray(0, this._lineCount), gl.DYNAMIC_DRAW);
    gl.enableVertexAttribArray(this._pos);
    gl.vertexAttribPointer(this._pos, 2, gl.FLOAT, false, 0, 0);
    gl.lineWidth(lineWidth);
    gl.drawArrays(gl.LINES, 0, this._lineCount / 2);
  }

  destroy() {
    const gl = this.gl;
    if (this.program) gl.deleteProgram(this.program);
    if (this.buf) gl.deleteBuffer(this.buf);
  }
}
