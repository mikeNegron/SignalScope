// Histogram renderer - instanced rectangles, one per bin.
// Per-instance data: x position (0-1 normalized), height (0-1 normalized), color.

import { _compile, _link, hexRGBA } from './TextRenderer.js';
import { ML, MR, MB } from './GridRenderer.js';

const VERT = `#version 300 es
in vec2  a_quad;    // unit quad (0,0)-(1,1)
in float a_binX;    // bin left edge, 0..1 normalized
in float a_height;  // bar height, 0..1 normalized
in vec3  a_color;   // RGB
uniform vec2 u_res;
uniform float u_binW;  // single bin width fraction (plot-relative)
uniform vec4 u_plot;   // x0, y_top, x1, y_bot in pixels
out vec4 v_col;
void main() {
  float plotW = u_plot.z - u_plot.x;
  float plotH = u_plot.w - u_plot.y;
  float bw = u_binW * plotW * 0.9;  // 90% gap
  float bx = u_plot.x + a_binX * plotW + a_quad.x * bw;
  float by = u_plot.w - a_height * plotH * a_quad.y;
  vec2 ndc;
  ndc.x =  bx / u_res.x * 2.0 - 1.0;
  ndc.y = -(by / u_res.y * 2.0 - 1.0);
  gl_Position = vec4(ndc, 0.0, 1.0);
  v_col = vec4(a_color, 0.7);
}`;

const FRAG = `#version 300 es
precision mediump float;
in vec4 v_col;
out vec4 outColor;
void main() { outColor = v_col; }`;

// Stride per instance: binX(1) + height(1) + color(3) = 5 floats
const ISTRIDE = 5;

export class HistogramRenderer {
  constructor(gl) {
    this.gl = gl;
    this.program = _link(gl,
      _compile(gl, gl.VERTEX_SHADER, VERT),
      _compile(gl, gl.FRAGMENT_SHADER, FRAG)
    );
    // Unit quad: triangle strip (0,0),(1,0),(0,1),(1,1)
    this.quadBuf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, this.quadBuf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([0,0, 1,0, 0,1, 1,1]), gl.STATIC_DRAW);
    this.instBuf = gl.createBuffer();
    this._instData = new Float32Array(256 * ISTRIDE);
    this.dpr = 1;

    const p = this.program;
    this._l = {
      aQuad:   gl.getAttribLocation(p, 'a_quad'),
      aBinX:   gl.getAttribLocation(p, 'a_binX'),
      aHeight: gl.getAttribLocation(p, 'a_height'),
      aColor:  gl.getAttribLocation(p, 'a_color'),
      uRes:    gl.getUniformLocation(p, 'u_res'),
      uBinW:   gl.getUniformLocation(p, 'u_binW'),
      uPlot:   gl.getUniformLocation(p, 'u_plot'),
    };
  }

  draw(w, h, data) {
    if (!data || data.length === 0) return;
    const gl = this.gl;
    const N = data.length;
    const d = this.dpr;
    const plot = [ML * d, 0, w - MR * d, h - MB * d];

    // Build instance data
    if (this._instData.length < N * ISTRIDE) {
      this._instData = new Float32Array(N * ISTRIDE * 2);
    }
    const inst = this._instData;
    for (let i = 0; i < N; i++) {
      const t = i / N;             // x normalized
      const ht = data[i];          // height normalized (0-1)
      const tt = Math.abs(t - 0.5) * 2; // 0 at center, 1 at edges
      const r = (tt * 200) | 0;
      const g = ((1 - tt) * 180) | 0;
      const b = ((1 - tt) * 255) | 0;
      const o = i * ISTRIDE;
      inst[o]   = t;
      inst[o+1] = ht;
      inst[o+2] = r / 255;
      inst[o+3] = g / 255;
      inst[o+4] = b / 255;
    }

    gl.useProgram(this.program);
    const l = this._l;
    gl.uniform2f(l.uRes, w, h);
    gl.uniform1f(l.uBinW, 1.0 / N);
    gl.uniform4fv(l.uPlot, plot);

    // Upload instance data
    gl.bindBuffer(gl.ARRAY_BUFFER, this.instBuf);
    gl.bufferData(gl.ARRAY_BUFFER, inst.subarray(0, N * ISTRIDE), gl.DYNAMIC_DRAW);

    const FSIZE = 4;
    const S = ISTRIDE * FSIZE;

    // Per-vertex: quad
    gl.bindBuffer(gl.ARRAY_BUFFER, this.quadBuf);
    gl.enableVertexAttribArray(l.aQuad);
    gl.vertexAttribPointer(l.aQuad, 2, gl.FLOAT, false, 0, 0);
    gl.vertexAttribDivisor(l.aQuad, 0);

    // Per-instance
    gl.bindBuffer(gl.ARRAY_BUFFER, this.instBuf);
    const setI = (loc, size, off) => {
      gl.enableVertexAttribArray(loc);
      gl.vertexAttribPointer(loc, size, gl.FLOAT, false, S, off * FSIZE);
      gl.vertexAttribDivisor(loc, 1);
    };
    setI(l.aBinX,   1, 0);
    setI(l.aHeight, 1, 1);
    setI(l.aColor,  3, 2);

    gl.enable(gl.BLEND);
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
    gl.drawArraysInstanced(gl.TRIANGLE_STRIP, 0, 4, N);
    gl.disable(gl.BLEND);

    // Reset divisors so subsequent renderers inherit per-vertex semantics
    gl.vertexAttribDivisor(l.aQuad,   0);
    gl.vertexAttribDivisor(l.aBinX,   0);
    gl.vertexAttribDivisor(l.aHeight, 0);
    gl.vertexAttribDivisor(l.aColor,  0);
  }

  destroy() {
    const gl = this.gl;
    if (this.quadBuf) gl.deleteBuffer(this.quadBuf);
    if (this.instBuf) gl.deleteBuffer(this.instBuf);
    if (this.program) gl.deleteProgram(this.program);
  }
}
