// Spectrum line strip renderer using a 1D R32F texture.
// Vertex shader reads dB values from the texture, mapping bin index -> x,
// dB value -> y. Two passes: gradient fill (TRIANGLE_STRIP) + line (LINE_STRIP).
// Peak detection and labels are rendered by OverlayRenderer / GPURenderer.

import { _compile, _link, hexRGBA } from './TextRenderer.js';
import { T } from '../lib/tokens.js';
import { ML, MR, MT, MB } from './GridRenderer.js';
import { logK } from '../lib/freq.js';
import { enforceMinSpread } from '../lib/numeric-safe.js';

// Map a linear data position t∈[0,1] to its screen position. For linear axis
// this is a no-op; for log axis we map (t * fmax) -> log(...) / log(fmax/fmin).
// fmin/fmax = exp(-u_logK), so the input `t` is clamped to that ratio first
// to keep log() finite at very low bins.
const LOG_REMAP = `
float remapT(float t, float scale, float logK) {
  if (scale < 0.5) return t;
  float tClamped = max(t, exp(-logK));
  return log(tClamped) / logK + 1.0;
}`;

// Map an axis-space t (after log remap) into the visible viewport
// window [u_xMin, u_xMax]. With no zoom this is identity.
const VIEWPORT_REMAP = `
float viewportT(float tAxis, float xMin, float xMax) {
  return (tAxis - xMin) / (xMax - xMin);
}`;

// Line strip shader
const LINE_VERT = `#version 300 es
in float a_idx;
uniform sampler2D u_data;
uniform float u_len;
uniform float u_dBmin;
uniform float u_dBmax;
uniform vec4 u_plot;   // x0, y_top, x1, y_bot (panel-local px)
uniform vec2 u_res;
uniform float u_scale; // 0=linear, 1=log
uniform float u_logK;  // log(fmax/fmin) - only used when u_scale > 0
uniform float u_xMin;  // viewport start (axis-space, 0..1)
uniform float u_xMax;  // viewport end
${LOG_REMAP}
${VIEWPORT_REMAP}
void main() {
  float t = a_idx / u_len;
  float db = texture(u_data, vec2(t, 0.5)).r;
  db = clamp(db, u_dBmin, u_dBmax);
  float tAxis = remapT(t, u_scale, u_logK);
  float tScreen = viewportT(tAxis, u_xMin, u_xMax);
  float px = mix(u_plot.x, u_plot.z, tScreen);
  float py = mix(u_plot.y, u_plot.w, (u_dBmax - db) / (u_dBmax - u_dBmin));
  vec2 ndc = px / u_res * 2.0 - 1.0;
  ndc.y = -(py / u_res.y * 2.0 - 1.0);
  gl_Position = vec4(ndc, 0.0, 1.0);
}`;

// Fill triangle-strip shader
// Vertex i: even -> data position, odd -> bottom of plot.
const FILL_VERT = `#version 300 es
in float a_idx;
uniform sampler2D u_data;
uniform float u_len;
uniform float u_dBmin;
uniform float u_dBmax;
uniform vec4 u_plot;
uniform vec2 u_res;
uniform float u_scale;
uniform float u_logK;
uniform float u_xMin;
uniform float u_xMax;
${LOG_REMAP}
${VIEWPORT_REMAP}
void main() {
  float isBot = mod(a_idx, 2.0);
  float di    = floor(a_idx / 2.0);
  float t     = di / u_len;
  float db    = texture(u_data, vec2(t, 0.5)).r;
  db = clamp(db, u_dBmin, u_dBmax);
  float tAxis = remapT(t, u_scale, u_logK);
  float tScreen = viewportT(tAxis, u_xMin, u_xMax);
  float px = mix(u_plot.x, u_plot.z, tScreen);
  float py = isBot > 0.5
    ? u_plot.w
    : mix(u_plot.y, u_plot.w, (u_dBmax - db) / (u_dBmax - u_dBmin));
  vec2 ndc;
  ndc.x =  px / u_res.x * 2.0 - 1.0;
  ndc.y = -(py / u_res.y * 2.0 - 1.0);
  gl_Position = vec4(ndc, 0.0, 1.0);
}`;

const FRAG = `#version 300 es
precision mediump float;
uniform vec4 u_color;
out vec4 outColor;
void main() { outColor = u_color; }`;

const C_LINE = hexRGBA(T.primary, 1.0);
const C_FILL = hexRGBA(T.primary, 0.07);

export class SpectrumRenderer {
  constructor(gl) {
    this.gl = gl;
    this.lineProg = _link(gl, _compile(gl, gl.VERTEX_SHADER, LINE_VERT), _compile(gl, gl.FRAGMENT_SHADER, FRAG));
    this.fillProg = _link(gl, _compile(gl, gl.VERTEX_SHADER, FILL_VERT), _compile(gl, gl.FRAGMENT_SHADER, FRAG));
    this.idxBuf    = null; // indices 0..N-1 for LINE_STRIP
    this.fillIdxBuf = null; // indices 0..2N-1 for TRIANGLE_STRIP
    this.dataTex   = null;
    this._prevN    = 0;
    this._texW     = 0;
    this._uploadScratch = null;
    // GPUs cap texture dimensions (often 16384, sometimes higher). When the
    // FFT size exceeds this we downsample the spectrum into a texture of
    // exactly MAX_TEXTURE_SIZE width - no visible loss because the panel
    // has nowhere near that many pixels anyway, but keeps glTexImage2D
    // from rejecting the call.
    this._maxTex = gl.getParameter(gl.MAX_TEXTURE_SIZE) || 4096;
    this.dpr       = 1;

    const lp = this.lineProg;
    const fp = this.fillProg;
    this._ll = {
      aIdx: gl.getAttribLocation(lp, 'a_idx'),
      uData: gl.getUniformLocation(lp, 'u_data'),
      uLen:  gl.getUniformLocation(lp, 'u_len'),
      uMin:  gl.getUniformLocation(lp, 'u_dBmin'),
      uMax:  gl.getUniformLocation(lp, 'u_dBmax'),
      uPlot: gl.getUniformLocation(lp, 'u_plot'),
      uRes:  gl.getUniformLocation(lp, 'u_res'),
      uCol:  gl.getUniformLocation(lp, 'u_color'),
      uScale: gl.getUniformLocation(lp, 'u_scale'),
      uLogK:  gl.getUniformLocation(lp, 'u_logK'),
      uXMin: gl.getUniformLocation(lp, 'u_xMin'),
      uXMax: gl.getUniformLocation(lp, 'u_xMax'),
    };
    this._fl = {
      aIdx: gl.getAttribLocation(fp, 'a_idx'),
      uData: gl.getUniformLocation(fp, 'u_data'),
      uLen:  gl.getUniformLocation(fp, 'u_len'),
      uMin:  gl.getUniformLocation(fp, 'u_dBmin'),
      uMax:  gl.getUniformLocation(fp, 'u_dBmax'),
      uPlot: gl.getUniformLocation(fp, 'u_plot'),
      uRes:  gl.getUniformLocation(fp, 'u_res'),
      uCol:  gl.getUniformLocation(fp, 'u_color'),
      uScale: gl.getUniformLocation(fp, 'u_scale'),
      uLogK:  gl.getUniformLocation(fp, 'u_logK'),
      uXMin: gl.getUniformLocation(fp, 'u_xMin'),
      uXMax: gl.getUniformLocation(fp, 'u_xMax'),
    };
  }

  draw(w, h, data, settings, holdData) {
    // When peak hold is on AND the backend is sending the held trace alongside
    // the live frame, draw the held trace as the spectrum line. The waterfall
    // (which reads from `data` directly elsewhere) is unaffected.
    const useHold = !!(settings?.peakHold && holdData && holdData.length >= 2);
    const source  = useHold ? holdData : data;
    if (!source || source.length < 2) return;
    const gl = this.gl;
    const N = source.length;

    if (N !== this._prevN) this._rebuild(N);
    this._uploadData(source, N);

    const d = this.dpr;
    const plot = [ML * d, MT * d, w - MR * d, h - MB * d]; // x0, yTop, x1, yBot
    const res  = [w, h];
    // Two-sided forces linear (RenderSurface already enforces this on the
    // settings object, but we keep the guard here so this method is safe to
    // call standalone). LogK only matters when scale=log; computed once per
    // frame from the live sample rate.
    const isLog = settings?.freqScale === 'log';
    const k = isLog ? logK(settings.sampleRate) : 0;
    const xMin = settings?.viewXMin ?? 0;
    const xMax = settings?.viewXMax ?? 1;
    // Y-axis range (Flatten mode rewrites these from EMA-smoothed extrema).
    let dbLo = settings?.dBmin ?? -130;
    let dbHi = settings?.dBmax ?? 10;
    // Defense in depth: the shader does `(u_dBmax - db) / (u_dBmax -
    // u_dBmin)` and would NaN if dbLo === dbHi. The Flatten-EMA path
    // already enforces a 1 dB minimum (RenderSurface.jsx); 0.01 here
    // catches any future code path that bypasses that.
    ({ lo: dbLo, hi: dbHi } = enforceMinSpread(dbLo, dbHi, 0.01));

    // Fill pass
    gl.useProgram(this.fillProg);
    const fl = this._fl;
    gl.uniform1i(fl.uData, 0);
    gl.uniform1f(fl.uLen, N - 1);
    gl.uniform1f(fl.uMin, dbLo);
    gl.uniform1f(fl.uMax, dbHi);
    gl.uniform4fv(fl.uPlot, plot);
    gl.uniform2fv(fl.uRes, res);
    gl.uniform4fv(fl.uCol, C_FILL);
    gl.uniform1f(fl.uScale, isLog ? 1 : 0);
    gl.uniform1f(fl.uLogK, k);
    gl.uniform1f(fl.uXMin, xMin);
    gl.uniform1f(fl.uXMax, xMax);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.dataTex);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.fillIdxBuf);
    gl.enableVertexAttribArray(fl.aIdx);
    gl.vertexAttribPointer(fl.aIdx, 1, gl.FLOAT, false, 0, 0);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, N * 2);
    gl.disable(gl.BLEND);

    // Line pass
    gl.useProgram(this.lineProg);
    const ll = this._ll;
    gl.uniform1i(ll.uData, 0);
    gl.uniform1f(ll.uLen, N - 1);
    gl.uniform1f(ll.uMin, dbLo);
    gl.uniform1f(ll.uMax, dbHi);
    gl.uniform4fv(ll.uPlot, plot);
    gl.uniform2fv(ll.uRes, res);
    gl.uniform4fv(ll.uCol, C_LINE);
    gl.uniform1f(ll.uScale, isLog ? 1 : 0);
    gl.uniform1f(ll.uLogK, k);
    gl.uniform1f(ll.uXMin, xMin);
    gl.uniform1f(ll.uXMax, xMax);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.idxBuf);
    gl.enableVertexAttribArray(ll.aIdx);
    gl.vertexAttribPointer(ll.aIdx, 1, gl.FLOAT, false, 0, 0);
    gl.drawArrays(gl.LINE_STRIP, 0, N);
  }

  _rebuild(N) {
    const gl = this.gl;
    // Line indices
    if (this.idxBuf) gl.deleteBuffer(this.idxBuf);
    this.idxBuf = gl.createBuffer();
    const lineIdx = new Float32Array(N);
    for (let i = 0; i < N; i++) lineIdx[i] = i;
    gl.bindBuffer(gl.ARRAY_BUFFER, this.idxBuf);
    gl.bufferData(gl.ARRAY_BUFFER, lineIdx, gl.STATIC_DRAW);

    // Fill indices (interleaved data/bottom pairs)
    if (this.fillIdxBuf) gl.deleteBuffer(this.fillIdxBuf);
    this.fillIdxBuf = gl.createBuffer();
    const fillIdx = new Float32Array(N * 2);
    for (let i = 0; i < N; i++) { fillIdx[i*2] = i*2; fillIdx[i*2+1] = i*2+1; }
    gl.bindBuffer(gl.ARRAY_BUFFER, this.fillIdxBuf);
    gl.bufferData(gl.ARRAY_BUFFER, fillIdx, gl.STATIC_DRAW);

    // Data texture (1×W R32F). W is min(N, MAX_TEXTURE_SIZE) - for large
    // FFT sizes we downsample on upload so the call doesn't get rejected
    // with INVALID_VALUE on GPUs whose MAX_TEXTURE_SIZE is below N.
    const W = Math.min(N, this._maxTex);
    this._texW = W;
    if (W < N) {
      // Pre-allocate a scratch row sized to the texture, reused on every
      // upload to avoid per-frame GC pressure.
      this._uploadScratch = new Float32Array(W);
    } else {
      this._uploadScratch = null;
    }
    if (this.dataTex) gl.deleteTexture(this.dataTex);
    this.dataTex = gl.createTexture();
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.dataTex);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.R32F, W, 1, 0, gl.RED, gl.FLOAT, null);

    this._prevN = N;
  }

  _uploadData(data, N) {
    const gl = this.gl;
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.dataTex);
    if (this._uploadScratch) {
      // Max-over-stride downsample: for each output bin, take the loudest
      // input bin in its range. Preserves spectral peaks (an averaging
      // resample would blur narrow tones into the noise floor).
      const out = this._uploadScratch;
      const W = this._texW;
      const stride = N / W;
      for (let i = 0; i < W; i++) {
        const start = (i * stride) | 0;
        const end = Math.min(N, ((i + 1) * stride) | 0);
        let m = data[start];
        for (let j = start + 1; j < end; j++) {
          if (data[j] > m) m = data[j];
        }
        out[i] = m;
      }
      gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, W, 1, gl.RED, gl.FLOAT, out);
    } else {
      gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, N, 1, gl.RED, gl.FLOAT, data);
    }
  }

  destroy() {
    const gl = this.gl;
    if (this.idxBuf)    gl.deleteBuffer(this.idxBuf);
    if (this.fillIdxBuf) gl.deleteBuffer(this.fillIdxBuf);
    if (this.dataTex)   gl.deleteTexture(this.dataTex);
    if (this.lineProg)  gl.deleteProgram(this.lineProg);
    if (this.fillProg)  gl.deleteProgram(this.fillProg);
  }
}
