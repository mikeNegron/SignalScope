// Spectrogram waterfall renderer - adapted from views/spectrogram.jsx.
// Draws into the current viewport rather than owning a canvas.
// Ring-buffer LUMINANCE texture + colormap LUT; cursor overlay moves to OverlayRenderer.

import { _compile, _link } from './TextRenderer.js';
import { CMAPS } from '../lib/tokens.js';
import { logK } from '../lib/freq.js';
import { makePhaseEstimator } from '../lib/phase-estimator.js';

const VERT = `#version 300 es
in vec2 a_pos;
out vec2 v_uv;
void main() {
  v_uv = vec2(a_pos.x * 0.5 + 0.5, 0.5 - a_pos.y * 0.5);
  gl_Position = vec4(a_pos, 0.0, 1.0);
}`;

const FRAG = `#version 300 es
precision highp float;
uniform sampler2D u_data;
uniform sampler2D u_cmap;
uniform float u_brightness;
uniform float u_contrast;
uniform float u_head;
uniform float u_scrollSpeed;  // 1 when smooth-scroll off; actual when on
uniform float u_phase;        // [0,1] fractional progress through current FFT interval (0 when off)
uniform float u_maxRows;
uniform float u_visible;
uniform float u_screenH;
uniform float u_scale; // 0=linear, 1=log
uniform float u_logK;  // log(fmax/fmin) - only used when u_scale > 0
uniform float u_xMin;  // viewport start (axis-space, 0..1)
uniform float u_xMax;  // viewport end
uniform float u_yMin;  // time viewport start, 0=newest, 1=oldest visible
uniform float u_yMax;
in vec2 v_uv;
out vec4 outColor;
void main() {
  // Screen-y -> row index. Without zoom (yMin=0, yMax=1), v_uv.y * screenH
  // gives the row directly. With zoom, the screen height covers a smaller
  // chunk of the texture rows so each pixel maps to a fractional row.
  float yT = u_yMin + v_uv.y * (u_yMax - u_yMin);
  float row = yT * u_screenH;
  if (row >= u_visible) {
    outColor = vec4(0.043, 0.035, 0.035, 1.0);
    return;
  }
  // Screen-x -> axis-x via viewport window, then axis-x -> linear data-x
  // via the inverse log mapping. With no zoom xMin=0, xMax=1 -> identity.
  float tAxis = u_xMin + v_uv.x * (u_xMax - u_xMin);
  float u = u_scale < 0.5 ? tAxis : exp(u_logK * (tAxis - 1.0));
  // Newest row at y=yMin, oldest at y=yMax. Subtract row from u_head so
  // a fractional row interpolates between two real ring-buffer rows for
  // smooth time-axis zoom (the texture filter is LINEAR).
  // effRow shifts the screen-row coordinate by scrollSpeed*phase pixels,
  // clamping to 0 at the top so the newest data "stripe" stays pinned to
  // the top edge during the inter-arrival interval. effRow/u_scrollSpeed
  // then maps screen pixels to fractional texture rows; LINEAR sampling
  // interpolates between adjacent FFT rows. When smooth-scroll is off,
  // u_scrollSpeed=1 and u_phase=0, reducing this to mod(u_head-1-row,..).
  float effRow = max(0.0, row - u_scrollSpeed * u_phase);
  float idx = mod(u_head - 1.0 - effRow / u_scrollSpeed, u_maxRows);
  float db = texture(u_data, vec2(u, (idx + 0.5) / u_maxRows)).r;
  float val = clamp((db + u_brightness) * u_contrast, 0.0, 1.0);
  outColor = texture(u_cmap, vec2(val, 0.5));
}`;

export class SpectrogramRenderer {
  constructor(gl) {
    this.gl = gl;
    this.program = _link(gl,
      _compile(gl, gl.VERTEX_SHADER, VERT),
      _compile(gl, gl.FRAGMENT_SHADER, FRAG)
    );

    // Full-quad VBO (same as original)
    this.vbo = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, this.vbo);
    gl.bufferData(gl.ARRAY_BUFFER,
      new Float32Array([-1,-1, 1,-1, -1,1, -1,1, 1,-1, 1,1]),
      gl.STATIC_DRAW
    );

    const p = this.program;
    this._locs = {
      aPos:         gl.getAttribLocation(p, 'a_pos'),
      uData:        gl.getUniformLocation(p, 'u_data'),
      uCmap:        gl.getUniformLocation(p, 'u_cmap'),
      uBri:         gl.getUniformLocation(p, 'u_brightness'),
      uCon:         gl.getUniformLocation(p, 'u_contrast'),
      uHead:        gl.getUniformLocation(p, 'u_head'),
      uScrollSpeed: gl.getUniformLocation(p, 'u_scrollSpeed'),
      uPhase:       gl.getUniformLocation(p, 'u_phase'),
      uMaxRows:     gl.getUniformLocation(p, 'u_maxRows'),
      uVisible:     gl.getUniformLocation(p, 'u_visible'),
      uScreenH:     gl.getUniformLocation(p, 'u_screenH'),
      uScale:       gl.getUniformLocation(p, 'u_scale'),
      uLogK:        gl.getUniformLocation(p, 'u_logK'),
      uXMin:        gl.getUniformLocation(p, 'u_xMin'),
      uXMax:        gl.getUniformLocation(p, 'u_xMax'),
      uYMin:        gl.getUniformLocation(p, 'u_yMin'),
      uYMax:        gl.getUniformLocation(p, 'u_yMax'),
    };

    this.texData = null;
    this.texCmap = null;
    this.cmapName = null;

    this.glHead = 0;
    this.glCount = 0;
    this.glMaxRows = 0;
    this.glRowLen = 0;
    this.uploadBuf = null;
    // Cap the row width at MAX_TEXTURE_SIZE so huge FFT sizes don't get
    // rejected by texImage2D. We downsample with max-over-stride on
    // upload (preserves peaks, doesn't smear narrow tones).
    this._maxTex = gl.getParameter(gl.MAX_TEXTURE_SIZE) || 4096;

    this._lastSpecSeq = -1;
    this._lastHistorySeq = -1;
    this._lastHistoryReps = -1;
    this._lastW = 0;
    this._lastH = 0;
    // Smooth-scroll state. The estimator emits u_phase per draw; the
    // last-flag sentinel detects toggle-off edges to reset EMA cleanly.
    this._phaseEstimator = makePhaseEstimator();
    this._lastSmoothScroll = false;
  }

  // Called once per frame for the spectrogram panel.
  // Draws the current ring buffer even when data is null (freeze on disconnect).
  draw(w, h, data, settings, specSeq = 0) {
    const gl = this.gl;
    const { colorMap, brightness, contrast, scrollSpeed = 1,
            freqScale = 'linear', sampleRate = 48000,
            viewXMin = 0, viewXMax = 1,
            viewYMin = 0, viewYMax = 1,
            historyMode = false } = settings;
    const isLog = freqScale === 'log';
    const k = isLog ? logK(sampleRate) : 0;

    // Reset EMA on smooth-scroll toggle-off so a future on-flip starts fresh.
    if (this._lastSmoothScroll && !settings.smoothSpectrogram) {
      this._phaseEstimator.reset();
    }
    this._lastSmoothScroll = !!settings.smoothSpectrogram;

    // `data` is the full buf object: { spectrum, _history, ... }.
    // Two upload paths share the same texture and shader:
    //   - live mode: stream new rows from data.spectrum as they arrive
    //   - history mode: re-populate from data._history when a request
    //     response advances the history seq
    const spectrum = data?.spectrum;
    const hist     = data?._history;

    if (historyMode && hist && spectrum) {
      const dataLen = spectrum.length || 2048;
      const rl = Math.min(dataLen, this._maxTex);
      const mr = h + 50;
      if (rl !== this.glRowLen || mr !== this.glMaxRows
          || dataLen !== this._lastDataLen) {
        this._rebuildDataTex(rl, mr);
        this._lastDataLen = dataLen;
      }
      // Mirror the live-path replication policy so history-mode and
      // live-mode use the same texture layout. With smooth-scroll on,
      // 1 row per FFT; the shader handles magnification.
      const reps = settings.smoothSpectrogram ? 1 : Math.max(1, scrollSpeed | 0);
      if (hist.lastSeq !== this._lastHistorySeq
          || reps !== this._lastHistoryReps) {
        this._lastHistorySeq = hist.lastSeq;
        this._lastHistoryReps = reps;
        // The shader's `head - 1 - row` indexing maps screen row 0 to
        // the most recently uploaded texel, so the LAST upload lands at
        // screen top. We want offset 0 (newest in time) at screen top,
        // therefore upload in descending offset order - oldest first,
        // newest last. Reset the ring before re-uploading so partial
        // responses don't leave stale rows below the new ones.
        //
        // Replicate each row `scrollSpeed` times to match the live path,
        // which uploads each incoming spectrum `scrollSpeed` times.
        // Without this, history rows render at 1 row per texel while
        // live rows render at scrollSpeed texels per row, so toggling
        // into history mode appears to "compress" the spectrogram.
        this.glHead = 0;
        this.glCount = 0;
        const rowKeys = Object.keys(hist.rows)
            .map((s) => parseInt(s, 10))
            .sort((a, b) => b - a);
        for (const off of rowKeys) {
          const row = hist.rows[off];
          for (let s = 0; s < reps; s++) this._uploadRow(row);
        }
      }
    } else if (spectrum) {
      const dataLen = spectrum.length || 2048;
      const rl = Math.min(dataLen, this._maxTex);
      const mr = h + 50;
      if (rl !== this.glRowLen || mr !== this.glMaxRows
          || dataLen !== this._lastDataLen) {
        this._rebuildDataTex(rl, mr);
        this._lastDataLen = dataLen;
      }
      if (specSeq !== this._lastSpecSeq) {
        this._lastSpecSeq = specSeq;
        if (settings.smoothSpectrogram) {
          // Smooth-scroll path: 1 row per FFT in texture. The shader
          // expands each FFT across scrollSpeed screen pixels via
          // effRow/scrollSpeed. Replicating here would collapse the
          // shader's interpolation between adjacent FFTs (since the
          // texture would have identical adjacent rows).
          this._phaseEstimator.onArrival(performance.now());
          this._uploadRow(spectrum);
        } else {
          // Existing behavior: scrollSpeed copies provide vertical
          // magnification; head advances by scrollSpeed per FFT.
          for (let s = 0; s < scrollSpeed; s++) this._uploadRow(spectrum);
        }
      }
    }

    // Rebuild colormap texture if needed (colormap changes should still apply)
    this._rebuildCmap(colorMap);

    // Nothing in the ring buffer yet - nothing to draw
    if (!this.texData) return;

    // Set uniforms and draw. The dB->byte texture upload anchors -130 dBFS
    // at byte 0 and ~−20 dBFS at byte 255 (slope = 2.318/dB), so the shader
    // sees `db_norm ≈ (dB + 130) / 110`. The shader then computes
    // `val = (db_norm + bri) * con` for the colormap lookup.
    //
    // Default user knobs map to a fixed [-130..−20] window. Flatten mode
    // takes over those knobs and computes (bri, con) so the auto-ranged
    // [dBmin..dBmax] window maps exactly to val ∈ [0, 1] - equivalent to
    // y-axis auto-scaling on the spectrum panel.
    let bri, con;
    if (settings.weighting === 'flatten'
        && settings.dBmin != null && settings.dBmax != null
        && settings.dBmax > settings.dBmin) {
      const lo = settings.dBmin;
      const hi = settings.dBmax;
      bri = -(lo + 130) / 110;
      con = 110 / (hi - lo);
    } else {
      bri = (brightness - 50) / 100;
      con = contrast / 50;
    }
    const l = this._locs;

    gl.useProgram(this.program);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.texData);
    gl.activeTexture(gl.TEXTURE1);
    gl.bindTexture(gl.TEXTURE_2D, this.texCmap);

    gl.uniform1i(l.uData, 0);
    gl.uniform1i(l.uCmap, 1);
    gl.uniform1f(l.uBri, bri);
    gl.uniform1f(l.uCon, con);
    gl.uniform1f(l.uHead, this.glHead);
    // u_scrollSpeed=1 when off so the shader's effRow/u_scrollSpeed
    // degenerates to plain row, collapsing the formula to the original
    // mod(u_head - 1 - row, u_maxRows). When on, pass the actual
    // scrollSpeed so each FFT spans that many screen pixels.
    gl.uniform1f(l.uScrollSpeed,
      settings.smoothSpectrogram ? Math.max(1, scrollSpeed | 0) : 1);
    gl.uniform1f(l.uPhase,
      settings.smoothSpectrogram ? this._phaseEstimator.getPhase(performance.now()) : 0);
    gl.uniform1f(l.uMaxRows, this.glMaxRows);
    gl.uniform1f(l.uVisible, Math.min(this.glCount, h));
    gl.uniform1f(l.uScreenH, h);
    gl.uniform1f(l.uScale, isLog ? 1 : 0);
    gl.uniform1f(l.uLogK, k);
    gl.uniform1f(l.uXMin, viewXMin);
    gl.uniform1f(l.uXMax, viewXMax);
    gl.uniform1f(l.uYMin, viewYMin);
    gl.uniform1f(l.uYMax, viewYMax);

    gl.bindBuffer(gl.ARRAY_BUFFER, this.vbo);
    gl.enableVertexAttribArray(l.aPos);
    gl.vertexAttribPointer(l.aPos, 2, gl.FLOAT, false, 0, 0);
    gl.drawArrays(gl.TRIANGLES, 0, 6);

    // Frequency ruler at the bottom (DOM labels handled by GPURenderer via TextRenderer)
  }

  _rebuildDataTex(rl, mr) {
    const gl = this.gl;
    if (this.texData) gl.deleteTexture(this.texData);
    this.texData = gl.createTexture();
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.texData);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    // LINEAR filtering bilinearly interpolates both the freq axis (between
    // bins) and the time axis (between rows). Closes most of the visual gap
    // vs. Baudline at low FFT sizes, where NEAREST would otherwise produce
    // visible blocky artefacts. LUMINANCE/UNSIGNED_BYTE is filterable in
    // WebGL2 - same as the colormap texture below.
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.LUMINANCE, rl, mr, 0, gl.LUMINANCE, gl.UNSIGNED_BYTE, null);
    this.glRowLen = rl;
    this.glMaxRows = mr;
    this.glHead = 0;
    this.glCount = 0;
    this.uploadBuf = new Uint8Array(rl);
  }

  _rebuildCmap(name) {
    if (name === this.cmapName && this.texCmap) return;
    this.cmapName = name;
    const fn = CMAPS[name] ?? CMAPS.baudline;
    const rgb = new Uint8Array(256 * 3);
    for (let i = 0; i < 256; i++) {
      const [r, g, b] = fn(i / 255);
      rgb[i*3] = r; rgb[i*3+1] = g; rgb[i*3+2] = b;
    }
    const gl = this.gl;
    if (!this.texCmap) this.texCmap = gl.createTexture();
    gl.activeTexture(gl.TEXTURE1);
    gl.bindTexture(gl.TEXTURE_2D, this.texCmap);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGB, 256, 1, 0, gl.RGB, gl.UNSIGNED_BYTE, rgb);
  }

  _uploadRow(data) {
    const b = this.uploadBuf;
    const W = this.glRowLen;
    if (data.length === W) {
      // Direct path - no downsampling.
      for (let i = 0; i < W; i++) {
        const raw = ((data[i] + 130) * 2.318) | 0;
        b[i] = raw < 0 ? 0 : raw > 255 ? 255 : raw;
      }
    } else if (data.length > W) {
      // Max-over-stride downsample fused with dB->byte mapping.
      const stride = data.length / W;
      for (let i = 0; i < W; i++) {
        const start = (i * stride) | 0;
        const end = Math.min(data.length, ((i + 1) * stride) | 0);
        let m = data[start];
        for (let j = start + 1; j < end; j++) {
          if (data[j] > m) m = data[j];
        }
        const raw = ((m + 130) * 2.318) | 0;
        b[i] = raw < 0 ? 0 : raw > 255 ? 255 : raw;
      }
    } else {
      // data shorter than texture row (rare - data length shrunk after
      // the texture was sized). Fill the rest with 0 (silence).
      const len = data.length;
      for (let i = 0; i < len; i++) {
        const raw = ((data[i] + 130) * 2.318) | 0;
        b[i] = raw < 0 ? 0 : raw > 255 ? 255 : raw;
      }
      for (let i = len; i < W; i++) b[i] = 0;
    }
    const gl = this.gl;
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.texData);
    gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, this.glHead, W, 1,
      gl.LUMINANCE, gl.UNSIGNED_BYTE, b);
    this.glHead = (this.glHead + 1) % this.glMaxRows;
    if (this.glCount < this.glMaxRows) this.glCount++;
  }

  destroy() {
    const gl = this.gl;
    if (this.texData) gl.deleteTexture(this.texData);
    if (this.texCmap) gl.deleteTexture(this.texCmap);
    if (this.vbo) gl.deleteBuffer(this.vbo);
    if (this.program) gl.deleteProgram(this.program);
  }
}
