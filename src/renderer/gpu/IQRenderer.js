// IQ constellation scatter plot.
// Re/Im samples are uploaded as a VBO of interleaved (re, im) pairs
// and rendered as instanced small quads (point sprites via TRIANGLE_STRIP).

import { _compile, _link, hexRGBA } from './TextRenderer.js';
import { T } from '../lib/tokens.js';

const VERT = `#version 300 es
in vec2 a_quad;   // unit quad (-0.5,-0.5)-(0.5,0.5)
in vec2 a_iq;     // per-instance: (re, im)
uniform vec2 u_res;
uniform float u_scale;  // pixels per unit
uniform float u_ptSz;   // half-size of each dot in pixels
void main() {
  vec2 center = vec2(u_res.x * 0.5 + a_iq.x * u_scale,
                     u_res.y * 0.5 - a_iq.y * u_scale);
  vec2 pos = center + a_quad * u_ptSz;
  vec2 ndc;
  ndc.x =  pos.x / u_res.x * 2.0 - 1.0;
  ndc.y = -(pos.y / u_res.y * 2.0 - 1.0);
  gl_Position = vec4(ndc, 0.0, 1.0);
}`;

const FRAG = `#version 300 es
precision mediump float;
uniform vec4 u_color;
out vec4 outColor;
void main() { outColor = u_color; }`;

const C_DOT = hexRGBA(T.primary, 0.38);

export class IQRenderer {
  constructor(gl) {
    this.gl = gl;
    this.program = _link(gl,
      _compile(gl, gl.VERTEX_SHADER, VERT),
      _compile(gl, gl.FRAGMENT_SHADER, FRAG)
    );
    // unit quad for dot, centered at (0,0)
    this.quadBuf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, this.quadBuf);
    gl.bufferData(gl.ARRAY_BUFFER,
      new Float32Array([-0.5,-0.5, 0.5,-0.5, -0.5,0.5, 0.5,0.5]),
      gl.STATIC_DRAW
    );
    this.iqBuf = gl.createBuffer();
    this._prevN = 0;
    this._buf = null;

    const p = this.program;
    this._l = {
      aQuad:  gl.getAttribLocation(p, 'a_quad'),
      aIQ:    gl.getAttribLocation(p, 'a_iq'),
      uRes:   gl.getUniformLocation(p, 'u_res'),
      uScale: gl.getUniformLocation(p, 'u_scale'),
      uPtSz:  gl.getUniformLocation(p, 'u_ptSz'),
      uColor: gl.getUniformLocation(p, 'u_color'),
    };
  }

  draw(w, h, iqData) {
    if (!iqData) return;
    const gl = this.gl;
    const { re, im } = iqData;
    const N = re.length;
    if (N === 0) return;

    const sz = Math.min(w, h);
    const scale = sz / 2.5;

    // Build interleaved re/im buffer
    if (!this._buf || this._buf.length < N * 2) {
      this._buf = new Float32Array(N * 2);
    }
    for (let i = 0; i < N; i++) {
      this._buf[i*2]   = re[i];
      this._buf[i*2+1] = im[i];
    }

    const l = this._l;
    gl.useProgram(this.program);
    gl.uniform2f(l.uRes, w, h);
    gl.uniform1f(l.uScale, scale);
    gl.uniform1f(l.uPtSz, 0.8); // half-size -> 1.6px dot matching original
    gl.uniform4fv(l.uColor, C_DOT);

    // Upload IQ buffer
    gl.bindBuffer(gl.ARRAY_BUFFER, this.iqBuf);
    gl.bufferData(gl.ARRAY_BUFFER, this._buf.subarray(0, N * 2), gl.DYNAMIC_DRAW);

    // Per-vertex: unit quad
    gl.bindBuffer(gl.ARRAY_BUFFER, this.quadBuf);
    gl.enableVertexAttribArray(l.aQuad);
    gl.vertexAttribPointer(l.aQuad, 2, gl.FLOAT, false, 0, 0);
    gl.vertexAttribDivisor(l.aQuad, 0);

    // Per-instance: IQ sample
    gl.bindBuffer(gl.ARRAY_BUFFER, this.iqBuf);
    gl.enableVertexAttribArray(l.aIQ);
    gl.vertexAttribPointer(l.aIQ, 2, gl.FLOAT, false, 0, 0);
    gl.vertexAttribDivisor(l.aIQ, 1);

    gl.enable(gl.BLEND);
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
    gl.drawArraysInstanced(gl.TRIANGLE_STRIP, 0, 4, N);
    gl.disable(gl.BLEND);

    // Reset divisors so other renderers are not affected
    gl.vertexAttribDivisor(l.aQuad, 0);
    gl.vertexAttribDivisor(l.aIQ, 0);
  }

  destroy() {
    const gl = this.gl;
    if (this.quadBuf) gl.deleteBuffer(this.quadBuf);
    if (this.iqBuf)   gl.deleteBuffer(this.iqBuf);
    if (this.program) gl.deleteProgram(this.program);
  }
}
