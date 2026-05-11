// Phase plot renderer - maps radians [-π, +π] on the Y axis.
// Same data-texture line-strip pattern as SpectrumRenderer / WaveformRenderer.

import { _compile, _link, hexRGBA } from './TextRenderer.js';
import { T } from '../lib/tokens.js';
import { ML, MR, MT, MB } from './GridRenderer.js';

const LINE_VERT = `#version 300 es
in float a_idx;
uniform sampler2D u_data;
uniform float u_len;
uniform vec4 u_plot;  // x0, y_top, x1, y_bot
uniform vec2 u_res;
void main() {
  float t     = a_idx / u_len;
  float phase = texture(u_data, vec2(t, 0.5)).r;  // radians, [-π, π]
  float norm  = clamp(-phase / 3.14159265, -1.0, 1.0) * 0.5 + 0.5; // 0=bottom, 1=top
  float px = mix(u_plot.x, u_plot.z, t);
  float py = mix(u_plot.y, u_plot.w, 1.0 - norm);  // y_top at phase=+π
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

const C_LINE = hexRGBA(T.purple, 1.0);

export class PhaseRenderer {
  constructor(gl) {
    this.gl = gl;
    this.program = _link(gl,
      _compile(gl, gl.VERTEX_SHADER, LINE_VERT),
      _compile(gl, gl.FRAGMENT_SHADER, FRAG)
    );
    this.idxBuf  = null;
    this.dataTex = null;
    this._prevN  = 0;
    this.dpr     = 1;
    const p = this.program;
    this._l = {
      aIdx:   gl.getAttribLocation(p, 'a_idx'),
      uData:  gl.getUniformLocation(p, 'u_data'),
      uLen:   gl.getUniformLocation(p, 'u_len'),
      uPlot:  gl.getUniformLocation(p, 'u_plot'),
      uRes:   gl.getUniformLocation(p, 'u_res'),
      uColor: gl.getUniformLocation(p, 'u_color'),
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
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.dataTex);
    const d = this.dpr;
    gl.uniform1i(l.uData, 0);
    gl.uniform1f(l.uLen, N - 1);
    // Reserve the same top/bottom margins as the spectrum panel so the
    // gridline/label area at the top (π) and bottom (freq ticks) stays
    // free of the trace.
    gl.uniform4f(l.uPlot, ML * d, MT * d, w - MR * d, h - MB * d);
    gl.uniform2f(l.uRes, w, h);
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
