// GPURenderer - WebGL2 context manager and frame orchestrator.
// Owns all sub-renderers and drives the single RAF loop.
// Called by RenderSurface with layout rects, data, and settings each frame.

import { TextRenderer }       from './TextRenderer.js';
import { GridRenderer }        from './GridRenderer.js';
import { SpectrogramRenderer } from './SpectrogramRenderer.js';
import { SpectrumRenderer }    from './SpectrumRenderer.js';
import { WaveformRenderer }    from './WaveformRenderer.js';
import { PhaseRenderer }       from './PhaseRenderer.js';
import { HistogramRenderer }   from './HistogramRenderer.js';
import { IQRenderer }          from './IQRenderer.js';
import { OverlayRenderer }     from './OverlayRenderer.js';
import { T }                   from '../lib/tokens.js';

// Stable iteration order - avoids Object.entries() allocation per frame.
const FRAME_PANEL_KEYS = ['spectrogram', 'spectrum', 'waveform', 'iq'];

// Spectrogram ruler strip - opaque dark band at the panel bottom with light
// text on top. Independent of the colormap so contrast is constant. Matches
// T.bg / T.text in tokens.js.
const SPECTRO_RULER_BG = [0.035, 0.035, 0.043, 1.0]; // T.bg  #09090B

export class GPURenderer {
  constructor(canvas) {
    const gl = canvas.getContext('webgl2', {
      alpha: false, depth: false, stencil: false,
      antialias: true, desynchronized: true,
      // Needed for exportPNG (canvas.toBlob) - without this the drawing
      // buffer is invalidated after compositing and toBlob captures empty
      // pixels. Tradeoff is a small constant cost (driver can't reuse the
      // buffer between frames). Acceptable for a 60fps signal scope.
      preserveDrawingBuffer: true,
    });
    if (!gl) throw new Error('WebGL2 not available');
    this.gl = gl;
    // Logical -> physical pixel scale (HiDPI). Updated by RenderSurface via
    // setDpr() and propagated to child renderers so they can scale margins,
    // text size, and line offsets properly.
    this.dpr = 1;

    this.text      = new TextRenderer(gl);
    this.grid      = new GridRenderer(gl, this.text);
    this.spectro   = new SpectrogramRenderer(gl);
    this.spectrum  = new SpectrumRenderer(gl);
    this.waveform  = new WaveformRenderer(gl);
    this.phase     = new PhaseRenderer(gl);
    this.histogram = new HistogramRenderer(gl);
    this.iq        = new IQRenderer(gl);
    this.overlay   = new OverlayRenderer(gl);
  }

  // Propagate DPR to all child renderers so they can scale CSS-pixel constants
  // (margins, text size, label offsets) into the physical-pixel WebGL viewport.
  setDpr(dpr) {
    this.dpr = dpr;
    this.text.dpr      = dpr;
    this.grid.dpr      = dpr;
    this.spectrum.dpr  = dpr;
    this.waveform.dpr  = dpr;
    this.phase.dpr     = dpr;
    this.histogram.dpr = dpr;
    this.iq.dpr        = dpr;
    this.overlay.dpr   = dpr;
  }

  // Main render call. layout = { key: { x, y, w, h, visible } } in canvas pixels.
  // data = { spectrum, waveform, iq, phase, histogram } (Float32Arrays or null).
  // settings = { sampleRate, colorMap, brightness, contrast, scrollSpeed,
  //              showPeaks, periodicityBars, fundamentalFreq,
  //              activeTab }. (cursorF1/F2 are rendered DOM-side in
  //              RenderSurface - see the CursorOverlay component.)
  frame(layout, data, settings) {
    const gl = this.gl;
    const cw = gl.canvas.width;
    const ch = gl.canvas.height;

    gl.viewport(0, 0, cw, ch);
    gl.clearColor(0.035, 0.035, 0.043, 1.0);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.enable(gl.SCISSOR_TEST);

    const sr = settings.sampleRate;
    const cf = settings.centerFreq || 0;
    const tw = !!settings.spectrumTwoSided;
    // Two-sided forces linear (RenderSurface already enforces this on the
    // settings object; defensive in case someone calls frame() directly).
    const sc = !tw && settings.freqScale === 'log' ? 'log' : 'linear';
    // Viewport window for the spectrum/spectrogram x-axis. [0,1] = full
    // axis; the wheel/drag handlers in RenderSurface narrow this when
    // the user pauses + zooms.
    const xMn = settings.viewXMin ?? 0;
    const xMx = settings.viewXMax ?? 1;
    // Y-axis dB range. RenderSurface rewrites these per-frame when Flatten
    // is on; otherwise they stay at the historical -130 / 10 defaults.
    const dbLo = settings.dBmin ?? -130;
    const dbHi = settings.dBmax ?? 10;
    const { activeTab = 'spectrum' } = settings;

    // Peaks come from the backend SpectrumStats frame. When hold is active
    // the picker on the C++ side runs against the held trace, so the markers
    // anchor to the steady envelope and stop jumping during bursts.
    const peaks = (settings.showPeaks && data.peaks && data.peaks.length)
      ? data.peaks : [];

    for (let i = 0; i < FRAME_PANEL_KEYS.length; i++) {
      const key  = FRAME_PANEL_KEYS[i];
      const rect = layout[key];
      if (!rect || !rect.visible || rect.w < 1 || rect.h < 1) continue;

      // WebGL Y is bottom-up; DOM Y is top-down.
      const glY = ch - rect.y - rect.h;
      gl.scissor(rect.x, glY, rect.w, rect.h);
      gl.viewport(rect.x, glY, rect.w, rect.h);

      const { w, h } = rect;

      switch (key) {
        case 'spectrogram':
          // Pass the full data object so the spectrogram can branch
          // between live (data.spectrum) and history-scroll mode
          // (data._history) sources.
          this.spectro.draw(w, h, data, settings, data.specSeq ?? 0);
          // f1/f2 cursors are rendered as DOM in RenderSurface - they update
          // only on click so a per-frame GPU pass is wasted work.
          this._drawSpectroRuler(w, h);
          this.text.flush(w, h);
          break;

        case 'spectrum':
          if (activeTab === 'histogram') {
            this.grid.draw(w, h, sr, 'histogram');
            this.histogram.draw(w, h, data.histogram);
            this.text.flush(w, h);
          } else if (activeTab === 'phase') {
            this.grid.draw(w, h, sr, 'phase', cf, tw, sc, xMn, xMx);
            this.phase.draw(w, h, data.phase);
            this.text.flush(w, h);
          } else {
            // Spectrum view
            this.grid.draw(w, h, sr, 'spectrum', cf, tw, sc, xMn, xMx, dbLo, dbHi);
            this.spectrum.draw(w, h, data.spectrum, settings, data.spectrumHold);
            if (peaks.length > 0) {
              this.overlay.drawPeaks(this.text, w, h, sr, peaks, cf, tw, sc, xMn, xMx, dbLo, dbHi);
            }
            if (settings.periodicityBars && settings.fundamentalFreq > 0) {
              this.overlay.drawHarmonics(
                this.text, w, h, sr, settings.fundamentalFreq, cf, tw, sc, xMn, xMx);
            }
            this.text.flush(w, h);
          }
          break;

        case 'waveform':
          this.grid.draw(w, h, sr, 'waveform');
          this.waveform.draw(w, h, data.waveform);
          this.text.flush(w, h);
          break;

        case 'iq':
          this.grid.drawIQGrid(w, h);
          this.iq.draw(w, h, data.iq);
          this.text.flush(w, h);
          break;
      }
    }

    gl.disable(gl.SCISSOR_TEST);
  }

  // Spectrogram frequency ruler
  // The waterfall paints over the entire panel and the colormap can be any
  // hue, so we still paint a dedicated dark strip at the bottom for label
  // contrast - but the labels themselves now render as React DOM in the
  // <SpectroRulerLabels> overlay (RenderSurface.jsx). This gives them the
  // same font/kerning/antialiasing as the rest of the UI, and they only
  // re-render when sample rate / center freq / scale changes.
  _drawSpectroRuler(w, h /*, sr, centerFreq, twoSided, scale */) {
    const d = this.dpr;
    const stripH = 22 * d;
    this.overlay._drawQuad(0, h - stripH, w, h, w, h, SPECTRO_RULER_BG);
  }

  destroy() {
    this.text.destroy();
    this.grid.destroy();
    this.spectro.destroy();
    this.spectrum.destroy();
    this.waveform.destroy();
    this.phase.destroy();
    this.histogram.destroy();
    this.iq.destroy();
    this.overlay.destroy();
    const ext = this.gl.getExtension('WEBGL_lose_context');
    if (ext) ext.loseContext();
  }
}
