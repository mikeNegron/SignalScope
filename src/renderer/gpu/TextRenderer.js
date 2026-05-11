// Glyph atlas + instanced text rendering.
// All text from all panels is enqueued per-frame and flushed in one
// instanced draw call. Coordinates are in the current viewport's local
// pixel space (0,0 = top-left), matching the WebGL viewport set by GPURenderer.

const VERT = `#version 300 es
in vec2 a_quad;
in vec2 a_pos;
in vec4 a_uv;
in vec4 a_color;
in float a_scale;
uniform vec2 u_res;
uniform vec2 u_atlasSz;
out vec2 v_tc;
out vec4 v_col;
void main() {
  vec2 sizePx = (a_uv.zw - a_uv.xy) * u_atlasSz * a_scale;
  vec2 p = a_pos + a_quad * sizePx;
  vec2 ndc = p / u_res * 2.0 - 1.0;
  ndc.y = -ndc.y;
  gl_Position = vec4(ndc, 0.0, 1.0);
  v_tc = mix(a_uv.xy, a_uv.zw, a_quad);
  v_col = a_color;
}`;

const FRAG = `#version 300 es
precision mediump float;
uniform sampler2D u_atlas;
in vec2 v_tc;
in vec4 v_col;
out vec4 outColor;
void main() {
  outColor = v_col * texture(u_atlas, v_tc).a;
}`;

const CHARS = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz.+-/:()%π ';
const CELL = 16; // atlas cell size in px (also baseline glyph render size at scale 1.0)
// Atlas is rendered at CELL*ATLAS_OVERSAMPLE for HiDPI sharpness. 2× is the
// sweet spot at typical render sizes (CELL*0.8*dpr); we then generate mipmaps
// so larger downscales don't soften strokes.
const ATLAS_OVERSAMPLE = 2;
const COLS = 16;
const STRIDE = 11; // pos(2) + uv(4) + color(4) + scale(1) floats per instance
const FONT_WEIGHT = 500; // medium - matches the visual weight of UI labels (which sit at 400-600)

export class TextRenderer {
  constructor(gl) {
    this.gl = gl;
    this.glyphs = {};
    this.atlasW = 0;
    this.atlasH = 0;
    this.atlas = null;
    this.program = null;
    this.quadBuf = null;
    this.instBuf = null;
    this._data = new Float32Array(512 * STRIDE);
    this._count = 0;
    this._locs = {};
    // DPR scale - set externally by GPURenderer.setDpr(). All text scales and
    // measurements multiply by this so labels render at consistent CSS size
    // regardless of the device pixel ratio (the WebGL viewport is in physical px).
    this.dpr = 1;
    this._buildAtlas();
    this._buildProgram();

    // Google Fonts (display=swap) may not have IBM Plex Mono loaded when the
    // constructor runs - the atlas above will have rendered the system fallback
    // monospace. Once the font is actually available, rebuild the atlas so
    // labels match the rest of the UI (cursor tab, controls, etc.).
    this._ensureFont();
  }

  _ensureFont() {
    if (typeof document === 'undefined' || !document.fonts || !document.fonts.ready) return;
    // IBM Plex Mono is the primary face loaded from Google Fonts; if it's
    // already there the initial atlas is already correct.
    const probe = `${FONT_WEIGHT} ${CELL - 3}px "IBM Plex Mono"`;
    if (document.fonts.check && document.fonts.check(probe)) return;
    document.fonts.ready
      .then(() => {
        if (!document.fonts.check || document.fonts.check(probe)) {
          this._rebuildAtlas();
        }
      })
      .catch(() => {});
  }

  _rebuildAtlas() {
    const gl = this.gl;
    if (this.atlas) gl.deleteTexture(this.atlas);
    this.atlas = null;
    this.glyphs = {};
    this._buildAtlas();
  }

  _buildAtlas() {
    const gl = this.gl;
    const ROWS = Math.ceil(CHARS.length / COLS);
    // Logical atlas size - the dimension the shader uses to compute output size.
    // The actual texture is rendered at OVERSAMPLE× this for HiDPI sharpness.
    const W = COLS * CELL;
    const H = ROWS * CELL;
    const OS = ATLAS_OVERSAMPLE;
    const physW = W * OS;
    const physH = H * OS;
    this.atlasW = W;
    this.atlasH = H;

    const oc = new OffscreenCanvas(physW, physH);
    const ctx = oc.getContext('2d');
    ctx.scale(OS, OS);
    // Match T.font in tokens.js so canvas labels share the UI's typeface.
    ctx.font = `${FONT_WEIGHT} ${CELL - 3}px ` +
      `'IBM Plex Mono','JetBrains Mono','Fira Code',monospace`;
    ctx.textBaseline = 'top';
    ctx.fillStyle = '#fff';

    for (let i = 0; i < CHARS.length; i++) {
      const col = i % COLS;
      const row = (i / COLS) | 0;
      const x = col * CELL;
      const y = row * CELL;
      ctx.fillText(CHARS[i], x + 1, y + 1);
      const m = ctx.measureText(CHARS[i]);
      this.glyphs[CHARS[i]] = {
        u0: x / W, v0: y / H,
        u1: (x + CELL) / W, v1: (y + CELL) / H,
        adv: Math.min(m.width + 1, CELL),
      };
    }

    this.atlas = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, this.atlas);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    // Mipmaps + trilinear filtering keep strokes crisp at any downscale ratio
    // (without them, a 5× bilinear shrink eats stroke weight and labels look thin).
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR_MIPMAP_LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, oc);
    gl.generateMipmap(gl.TEXTURE_2D);
  }

  _buildProgram() {
    const gl = this.gl;
    this.program = _link(gl,
      _compile(gl, gl.VERTEX_SHADER, VERT),
      _compile(gl, gl.FRAGMENT_SHADER, FRAG)
    );
    // unit quad: (0,0),(1,0),(0,1),(1,1) for TRIANGLE_STRIP
    this.quadBuf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, this.quadBuf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([0,0, 1,0, 0,1, 1,1]), gl.STATIC_DRAW);
    this.instBuf = gl.createBuffer();
    const p = this.program;
    this._locs = {
      quad:    gl.getAttribLocation(p, 'a_quad'),
      pos:     gl.getAttribLocation(p, 'a_pos'),
      uv:      gl.getAttribLocation(p, 'a_uv'),
      color:   gl.getAttribLocation(p, 'a_color'),
      scale:   gl.getAttribLocation(p, 'a_scale'),
      uRes:    gl.getUniformLocation(p, 'u_res'),
      uAtlSz:  gl.getUniformLocation(p, 'u_atlasSz'),
      uAtlas:  gl.getUniformLocation(p, 'u_atlas'),
    };
  }

  // Queue a string for rendering. x,y in current viewport pixels (top-left origin).
  // color: [r,g,b,a] floats 0-1. scale: logical (CSS-pixel) scale factor.
  // The internal scale is multiplied by this.dpr so callers can use CSS-px values.
  // Y is snapped to whole physical pixels for vertical sharpness; X is left
  // sub-pixel so labels that follow a cursor or scrub a frequency move smoothly
  // instead of "snapping" between pixel positions.
  enqueue(text, x, y, color, scale = 1.0) {
    const s = scale * this.dpr;
    const yp = Math.round(y);
    let cx = x;
    for (const ch of text) {
      const g = this.glyphs[ch] ?? this.glyphs[' '];
      if (!g) continue;
      const need = (this._count + 1) * STRIDE;
      if (need > this._data.length) {
        const next = new Float32Array(this._data.length * 2);
        next.set(this._data);
        this._data = next;
      }
      const o = this._count * STRIDE;
      this._data[o]    = cx;
      this._data[o+1]  = yp;
      this._data[o+2]  = g.u0; this._data[o+3] = g.v0;
      this._data[o+4]  = g.u1; this._data[o+5] = g.v1;
      this._data[o+6]  = color[0]; this._data[o+7] = color[1];
      this._data[o+8]  = color[2]; this._data[o+9] = color[3];
      this._data[o+10] = s;
      this._count++;
      cx += g.adv * s;
    }
  }

  // Measure text width in physical pixels at the given logical scale.
  measure(text, scale = 1.0) {
    const s = scale * this.dpr;
    let w = 0;
    for (const ch of text) {
      const g = this.glyphs[ch] ?? this.glyphs[' '];
      if (g) w += g.adv * s;
    }
    return w;
  }

  // Draw all enqueued glyphs. resW/resH = current viewport size in pixels.
  flush(resW, resH) {
    if (this._count === 0) return;
    const gl = this.gl;
    const l = this._locs;
    const FSIZE = 4;
    const S = STRIDE * FSIZE;

    gl.useProgram(this.program);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);

    gl.uniform2f(l.uRes, resW, resH);
    gl.uniform2f(l.uAtlSz, this.atlasW, this.atlasH);
    gl.uniform1i(l.uAtlas, 0);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.atlas);

    gl.bindBuffer(gl.ARRAY_BUFFER, this.instBuf);
    gl.bufferData(gl.ARRAY_BUFFER,
      this._data.subarray(0, this._count * STRIDE), gl.DYNAMIC_DRAW);

    // per-vertex: unit quad
    gl.bindBuffer(gl.ARRAY_BUFFER, this.quadBuf);
    gl.enableVertexAttribArray(l.quad);
    gl.vertexAttribPointer(l.quad, 2, gl.FLOAT, false, 0, 0);
    gl.vertexAttribDivisor(l.quad, 0);

    // per-instance attributes
    gl.bindBuffer(gl.ARRAY_BUFFER, this.instBuf);
    const setI = (loc, size, off) => {
      gl.enableVertexAttribArray(loc);
      gl.vertexAttribPointer(loc, size, gl.FLOAT, false, S, off * FSIZE);
      gl.vertexAttribDivisor(loc, 1);
    };
    setI(l.pos,   2, 0);
    setI(l.uv,    4, 2);
    setI(l.color, 4, 6);
    setI(l.scale, 1, 10);

    gl.drawArraysInstanced(gl.TRIANGLE_STRIP, 0, 4, this._count);

    gl.disable(gl.BLEND);
    // Reset instance divisors
    gl.vertexAttribDivisor(l.quad,  0);
    gl.vertexAttribDivisor(l.pos,   0);
    gl.vertexAttribDivisor(l.uv,    0);
    gl.vertexAttribDivisor(l.color, 0);
    gl.vertexAttribDivisor(l.scale, 0);
    this._count = 0;
  }

  destroy() {
    const gl = this.gl;
    if (this.atlas)   gl.deleteTexture(this.atlas);
    if (this.quadBuf) gl.deleteBuffer(this.quadBuf);
    if (this.instBuf) gl.deleteBuffer(this.instBuf);
    if (this.program) gl.deleteProgram(this.program);
  }
}

// Shared GL helpers
export function _compile(gl, type, src) {
  const s = gl.createShader(type);
  gl.shaderSource(s, src);
  gl.compileShader(s);
  if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) {
    console.error('Shader error:', gl.getShaderInfoLog(s), src.slice(0, 120));
    gl.deleteShader(s);
    return null;
  }
  return s;
}

export function _link(gl, vs, fs) {
  const p = gl.createProgram();
  gl.attachShader(p, vs);
  gl.attachShader(p, fs);
  gl.linkProgram(p);
  if (!gl.getProgramParameter(p, gl.LINK_STATUS)) {
    console.error('Program link error:', gl.getProgramInfoLog(p));
    gl.deleteProgram(p);
    return null;
  }
  gl.deleteShader(vs);
  gl.deleteShader(fs);
  return p;
}

// Convert '#RRGGBB' or '#RRGGBBAA' hex to [r,g,b,a] float array.
export function hexRGBA(hex, a = 1.0) {
  const h = hex.replace('#', '');
  return [
    parseInt(h.slice(0, 2), 16) / 255,
    parseInt(h.slice(2, 4), 16) / 255,
    parseInt(h.slice(4, 6), 16) / 255,
    h.length > 6 ? parseInt(h.slice(6, 8), 16) / 255 : a,
  ];
}
