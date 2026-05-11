// Waveform oscilloscope renderer - same data-texture line-strip pattern as
// SpectrumRenderer, but maps amplitude [-1, +1] on the Y axis.

import { _compile, _link, hexRGBA } from './TextRenderer.js';
import { T } from '../lib/tokens.js';
import { ML, MR } from './GridRenderer.js';

const LINE_VERT = `#version 300 es
in float a_idx;
uniform sampler2D u_data;
uniform float u_len;
uniform float u_ampScale; // h/2 * (1/scaleDiv)
uniform vec2 u_res;
uniform vec2 u_xRange; // left, right pixel bounds of the plot area
void main() {
  float t  = a_idx / u_len;
  float amp = texture(u_data, vec2(t, 0.5)).r;
  // Map t over the plot rectangle (excluding the left margin where the
  // amplitude labels live) so the trace can't draw over them.
  float px = mix(u_xRange.x, u_xRange.y, t);
  float py = u_res.y * 0.5 - amp * u_ampScale;
  py = clamp(py, 0.0, u_res.y);
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

const C_LINE = hexRGBA(T.success, 1.0);

export class WaveformRenderer {
  constructor(gl) {
    this.gl = gl;
    this.program = _link(gl,
      _compile(gl, gl.VERTEX_SHADER, LINE_VERT),
      _compile(gl, gl.FRAGMENT_SHADER, FRAG)
    );
    this.idxBuf  = null;
    this.dataTex = null;
    this._prevN  = 0;
    const p = this.program;
    this._l = {
      aIdx:     gl.getAttribLocation(p, 'a_idx'),
      uData:    gl.getUniformLocation(p, 'u_data'),
      uLen:     gl.getUniformLocation(p, 'u_len'),
      uAmpSc:   gl.getUniformLocation(p, 'u_ampScale'),
      uRes:     gl.getUniformLocation(p, 'u_res'),
      uColor:   gl.getUniformLocation(p, 'u_color'),
      uXRange:  gl.getUniformLocation(p, 'u_xRange'),
    };
  }

  draw(w, h, data) {
    if (!data || data.length < 2) return;
    const gl = this.gl;
    const N = data.length;
    if (N !== this._prevN) this._rebuild(N);
    this._uploadData(data, N);

    gl.useProgram(this.program);
    const l = this._l;
    const d = this.dpr ?? 1;
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.dataTex);
    gl.uniform1i(l.uData, 0);
    gl.uniform1f(l.uLen, N - 1);
    gl.uniform1f(l.uAmpSc, h / 2.2);
    gl.uniform2f(l.uRes, w, h);
    gl.uniform2f(l.uXRange, ML * d, w - MR * d);
    gl.uniform4fv(l.uColor, C_LINE);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.idxBuf);
    gl.enableVertexAttribArray(l.aIdx);
    gl.vertexAttribPointer(l.aIdx, 1, gl.FLOAT, false, 0, 0);
    gl.drawArrays(gl.LINE_STRIP, 0, N);
  }

  _rebuild(N) {
    const gl = this.gl;
    if (this.idxBuf) gl.deleteBuffer(this.idxBuf);
    this.idxBuf = gl.createBuffer();
    const idx = new Float32Array(N);
    for (let i = 0; i < N; i++) idx[i] = i;
    gl.bindBuffer(gl.ARRAY_BUFFER, this.idxBuf);
    gl.bufferData(gl.ARRAY_BUFFER, idx, gl.STATIC_DRAW);

    if (this.dataTex) gl.deleteTexture(this.dataTex);
    this.dataTex = gl.createTexture();
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.dataTex);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.R32F, N, 1, 0, gl.RED, gl.FLOAT, null);
    this._prevN = N;
  }

  _uploadData(data, N) {
    const gl = this.gl;
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.dataTex);
    gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, N, 1, gl.RED, gl.FLOAT, data);
  }

  destroy() {
    const gl = this.gl;
    if (this.idxBuf)  gl.deleteBuffer(this.idxBuf);
    if (this.dataTex) gl.deleteTexture(this.dataTex);
    if (this.program) gl.deleteProgram(this.program);
  }
}
