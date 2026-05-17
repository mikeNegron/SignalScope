// Single-canvas host for all analytical views.
// Owns one <canvas>, one WebGL2 context, one RAF loop, one compositor surface.
// DOM overlays (panel headers, drag handles, hidden-panel bars) are absolutely
// positioned on top of the canvas. React only handles layout and UI state.

import { useRef, useEffect, useState, useCallback, useMemo } from 'react';
import { GPURenderer } from '../gpu/GPURenderer.js';
import { T } from '../lib/tokens.js';
import { PANEL_KEYS } from '../hooks/use-panel-layout.js';
import { tToFreq, fmtFreqShort, freqToT } from '../lib/freq.js';
import { enforceMinSpread } from '../lib/numeric-safe.js';

// Layout constants - must match the overlay DOM elements below.
const HEADER_H  = 22; // visible panel title bar height (px)
const DRAG_H    = 6;  // drag-handle height between panels (px)

const PANEL_LABELS = {
  spectrogram: 'Spectrogram - Waterfall',
  spectrum:    'Spectrum',
  waveform:    'Waveform - Oscilloscope',
  iq:          'IQ Constellation',
};

// Stable empty-data sentinel - avoids `{}` allocations per frame when no data
// is available (disconnected backend, simulation paused, etc.).
const EMPTY_DATA = Object.freeze({
  spectrum: null, waveform: null, iq: null, phase: null, histogram: null,
  specSeq: 0, wavSeq: 0,
  peaks: [], noiseFloor: -120, fundamentalFreq: 0,
});

function computeLayout(containerW, containerH, panels, pFracs) {
  const visKeys = PANEL_KEYS.filter((k) => panels[k]);
  const hidKeys = PANEL_KEYS.filter((k) => !panels[k]);

  const numDrags  = Math.max(0, visKeys.length - 1);
  const totalPanelH = Math.max(0, containerH - numDrags * DRAG_H);
  const tot = visKeys.reduce((s, k) => s + (pFracs[k] || 0.25), 0) || 1;

  const layout = {};
  // Hidden panels take no real estate - they're re-enabled from the
  // control strip's panel toggles, not from an in-canvas bar.
  let y = 0;

  for (let i = 0; i < visKeys.length; i++) {
    const k = visKeys[i];
    const panelH = Math.round(((pFracs[k] || 0.25) / tot) * totalPanelH);

    if (i > 0) y += DRAG_H;

    // GPU content rect excludes the header bar
    const contentY = y + HEADER_H;
    const contentH = Math.max(1, panelH - HEADER_H);

    layout[k] = {
      x: 0, y: contentY, w: containerW, h: contentH,
      visible: true,
      // DOM overlay positions
      domY: y, domH: panelH, hasDrag: i > 0,
    };

    y += panelH;
  }

  for (const k of hidKeys) {
    layout[k] = { visible: false };
  }

  return { layout, visKeys, hidKeys };
}

export function RenderSurface({
  containerRef,   // from usePanelLayout - used by onDiv for drag delta calculation
  panels, pFracs, tog, onDiv,
  panelTitles,    // { key: JSX } - content of each panel's title area
  panelToolbars,  // { key: JSX } - right side of each panel header
  dataRef,        // ref whose .current = { spectrum, waveform, iq, phase, histogram, spectrumHold }
  // settings
  sampleRate, centerFreq = 0, spectrumTwoSided = false,
  colorMap, brightness, contrast, scrollSpeed,
  showPeaks, periodicityBars, fundamentalFreq, peakHold = false,
  freqScale = "linear",
  // "none" | "flatten" | "A-weight" | "C-weight". A/C-weight is applied
  // backend-side. Flatten is implemented here as an EMA-smoothed y-axis
  // auto-range - see settings.dBmin/dBmax computation in the RAF loop.
  weighting = "none",
  paused = false,
  viewXMin = 0,
  viewXMax = 1,
  onViewX,
  // Time-axis (y) viewport on the spectrogram. 0 = newest visible row,
  // 1 = oldest. With no zoom (0..1) the panel shows the most recent
  // panel-height rows. Narrowing this range zooms in time.
  viewYMin = 0,
  viewYMax = 1,
  onViewY,
  // Deep-history scroll. Only meaningful while paused: scrollOffset
  // is the number of rows back from the newest available row.
  scrollOffset = 0,
  onScrollOffset,
  historyCapacity = 4096,
  cursorF1, cursorF2, activeTab,
  onCursorMove,
}) {
  const _ownRef   = useRef(null);
  const _cRef     = containerRef || _ownRef; // use external ref if provided
  const canvasRef = useRef(null);
  const rendererRef  = useRef(null);
  const rafRef       = useRef(null);

  // Size of the canvas container in CSS pixels
  const [size, setSize] = useState({ w: 800, h: 600 });

  // dataRef is passed in directly - GPU RAF reads dataRef.current each frame.
  // settingsRef is mutated in place each render so the GPU RAF sees fresh values
  // without us allocating a new object every render.
  const settingsRef = useRef({
    sampleRate: 48000, centerFreq: 0, spectrumTwoSided: false,
    colorMap: 'baudline', brightness: 50, contrast: 70,
    scrollSpeed: 1, showPeaks: false, periodicityBars: false, fundamentalFreq: 0,
    peakHold: false, freqScale: "linear",
    weighting: "none",
    // Y-axis dB range. Defaults match the historical hardcoded constants
    // in SpectrumRenderer/GridRenderer/OverlayRenderer. Flatten mode rewrites
    // these per-frame from an EMA of the live spectrum's min/max.
    dBmin: -130, dBmax: 10,
    viewXMin: 0, viewXMax: 1, viewYMin: 0, viewYMax: 1,
    historyMode: false,
    cursorF1: null, cursorF2: null, activeTab: 'spectrum',
  });
  // EMA state for Flatten auto-range. dBmin/dBmax track the spectrum's
  // smoothed extrema so y-axis labels and color mapping don't twitch frame
  // to frame. Backend ships raw min/max in SpectrumStats; we only EMA
  // when statsSeq advances so the time constant tracks data updates,
  // not RAF ticks. Reset whenever weighting flips off Flatten.
  const autoRangeRef = useRef({ seeded: false, lo: -130, hi: 10, lastSeq: -1 });
  const layoutRef   = useRef({});
  // Stable physical-layout object reused every frame - fields are mutated in place
  // by the GPU RAF callback. Allocating a fresh object + spreads per frame creates
  // ~5 garbage objects × 60fps that add up under sustained data load.
  const physLayoutRef = useRef({
    spectrogram: { x: 0, y: 0, w: 0, h: 0, visible: false },
    spectrum:    { x: 0, y: 0, w: 0, h: 0, visible: false },
    waveform:    { x: 0, y: 0, w: 0, h: 0, visible: false },
    iq:          { x: 0, y: 0, w: 0, h: 0, visible: false },
  });

  const s = settingsRef.current;
  s.sampleRate = sampleRate;
  s.centerFreq = centerFreq;
  s.spectrumTwoSided = spectrumTwoSided;
  s.colorMap = colorMap;
  s.brightness = brightness;
  s.contrast = contrast;
  s.scrollSpeed = scrollSpeed;
  s.showPeaks = showPeaks;
  s.periodicityBars = periodicityBars;
  s.fundamentalFreq = fundamentalFreq;
  s.peakHold = peakHold;
  // Two-sided sources don't have a meaningful log axis (negative freqs);
  // force linear in that case so renderers don't need extra branching.
  s.freqScale = spectrumTwoSided ? "linear" : freqScale;
  s.viewXMin = viewXMin;
  s.viewXMax = viewXMax;
  s.viewYMin = viewYMin;
  s.viewYMax = viewYMax;
  // History mode is engaged whenever the user has scrolled away from
  // "newest" while paused - the spectrogram switches to display
  // request_history rows instead of the live ring.
  s.historyMode = paused && scrollOffset > 0;
  s.cursorF1 = cursorF1;
  s.cursorF2 = cursorF2;
  s.activeTab = activeTab;
  // When the user toggles off flatten, snap the axis back to defaults so
  // we don't carry the previous auto-range into the static view.
  if (s.weighting !== weighting) {
    s.weighting = weighting;
    if (weighting !== "flatten") {
      s.dBmin = -130; s.dBmax = 10;
      autoRangeRef.current.seeded = false;
      autoRangeRef.current.lastSeq = -1;
    }
  }

  // Recompute layout whenever size, panel visibility, or fracs change
  const { layout, visKeys, hidKeys } = useMemo(
    () => computeLayout(size.w, size.h, panels, pFracs),
    [size, panels, pFracs]
  );
  layoutRef.current = layout;

  useEffect(() => {
    const el = _cRef.current;
    if (!el) return;
    const ro = new ResizeObserver(([e]) => {
      const { width: w, height: h } = e.contentRect;
      if (w > 0 && h > 0) setSize({ w: Math.round(w), h: Math.round(h) });
    });
    ro.observe(el);
    return () => ro.disconnect();
  }, []);

  // Sync canvas physical size to CSS size (account for DPR).
  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const dpr = window.devicePixelRatio || 1;
    canvas.width  = Math.round(size.w * dpr);
    canvas.height = Math.round(size.h * dpr);
  }, [size]);

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    let renderer;
    try {
      renderer = new GPURenderer(canvas);
      rendererRef.current = renderer;
    } catch (e) {
      console.error('[RenderSurface] WebGL2 init failed:', e);
      return;
    }

    const dpr = window.devicePixelRatio || 1;
    renderer.setDpr(dpr);

    const frame = () => {
      const r = rendererRef.current;
      if (!r) return;
      const data      = dataRef.current ?? EMPTY_DATA;
      const settings  = settingsRef.current;
      const rawLayout = layoutRef.current;
      const phys      = physLayoutRef.current;

      // Scale layout rects from CSS pixels to physical pixels (DPR), updating
      // the pre-allocated physLayout in place.
      for (let i = 0; i < PANEL_KEYS.length; i++) {
        const k = PANEL_KEYS[i];
        const v = rawLayout[k];
        const p = phys[k];
        if (!v || !v.visible) { p.visible = false; continue; }
        p.visible = true;
        p.x = Math.round(v.x * dpr);
        p.y = Math.round(v.y * dpr);
        p.w = Math.round(v.w * dpr);
        p.h = Math.round(v.h * dpr);
      }

      // Flatten = y-axis auto-range. Backend hands us spec_min/spec_max
      // alongside the stats frame; we EMA them once per stats update
      // (statsSeq advance) so the time constant is tied to the data
      // rate, not RAF. Pad the bottom by 6 dB so the noise floor doesn't
      // flatten against the bottom edge, and the top by 3 dB so the
      // strongest peak isn't cropped against the upper grid line.
      if (settings.weighting === "flatten"
          && Number.isFinite(data.specMin) && Number.isFinite(data.specMax)
          && data.specMax > data.specMin) {
        const ar = autoRangeRef.current;
        if (data.statsSeq !== ar.lastSeq) {
          ar.lastSeq = data.statsSeq;
          if (!ar.seeded) {
            ar.lo = data.specMin; ar.hi = data.specMax; ar.seeded = true;
          } else {
            const ALPHA = 0.25; // ~4 stats updates to converge ≈ 60-130ms
            ar.lo = ALPHA * data.specMin + (1 - ALPHA) * ar.lo;
            ar.hi = ALPHA * data.specMax + (1 - ALPHA) * ar.hi;
          }
          // Defense-in-depth: even with the outer specMax > specMin guard,
          // a future code path or a manual ar mutation could collapse the
          // EMA to a single value, which would propagate through the
          // padding below into a degenerate dB range. Enforce a 1 dB
          // minimum spread on ar itself so the padding always starts from
          // a sane base.
          const spread = enforceMinSpread(ar.lo, ar.hi, 1);
          ar.lo = spread.lo;
          ar.hi = spread.hi;
        }
        settings.dBmin = ar.lo - 6;
        settings.dBmax = ar.hi + 3;
      }

      r.frame(phys, data, settings);
      rafRef.current = requestAnimationFrame(frame);
    };

    rafRef.current = requestAnimationFrame(frame);

    return () => {
      if (rafRef.current) cancelAnimationFrame(rafRef.current);
      renderer.destroy();
      rendererRef.current = null;
    };
  }, []); // only runs once - layout/data/settings are read from refs

  const handleClick = useCallback((e) => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const rect  = canvas.getBoundingClientRect();
    const cx    = e.clientX - rect.left;
    const cy    = e.clientY - rect.top;
    const lay   = layoutRef.current;
    const s     = settingsRef.current;
    const which = e.shiftKey ? 'f2' : 'f1';

    for (const [key, panel] of Object.entries(lay)) {
      if (!panel.visible) continue;
      // Hit-test the content area
      if (cy >= panel.y && cy < panel.y + panel.h &&
          cx >= panel.x && cx < panel.x + panel.w) {
        // Cursors only apply on views with a frequency x-axis: spectrogram
        // (always), and the spectrum-panel slot when its active tab is
        // 'spectrum' or 'phase'. The histogram (amplitude axis), waveform
        // (time-sample axis), and IQ (constellation, no x-axis) all have
        // no frequency meaning, so clicks are inert there - otherwise we'd
        // silently move f1/f2 to whatever the click position mapped to,
        // and the cursor would jump when the user tabs back to spectrum.
        if (key === 'spectrum'
            && s.activeTab !== 'spectrum' && s.activeTab !== 'phase') {
          break;
        }
        if (key === 'spectrogram' || key === 'spectrum') {
          // The frequency axis only spans the plot rectangle, NOT the full
          // panel width. The spectrum panel has 34px reserved on the left
          // for dB labels and 8px on the right; clicking inside the panel
          // but outside the plot must map relative to plot bounds, not the
          // panel bounds. Spectrogram has no left inset (no dB axis).
          const ML_CSS = 34, MR_CSS = 8;
          const plotLeft = panel.x + (key === 'spectrogram' ? 0 : ML_CSS);
          const plotWidth = panel.w
              - (key === 'spectrogram' ? 0 : ML_CSS) - MR_CSS;
          const tViewRaw = (cx - plotLeft) / plotWidth;
          const tView = Math.max(0, Math.min(1, tViewRaw));
          const tAxis = s.viewXMin + tView * (s.viewXMax - s.viewXMin);
          const freq = tToFreq(
            tAxis, s.sampleRate, s.centerFreq, s.spectrumTwoSided, s.freqScale);
          onCursorMove?.(freq, which);
        }
        break;
      }
    }
  }, [onCursorMove]);

  // Zoom interactions when paused
  // Drag       = marquee zoom: drag a rectangle and the viewport snaps to
  //              its left/right edges. A translucent overlay div tracks
  //              the drag so the user sees what they're selecting.
  // Alt+wheel  = continuous frequency-axis zoom anchored at the cursor.
  // Shift+wheel= time-axis zoom on the spectrogram (anchored at cursor).
  // Plain wheel on the spectrogram = deep-history scroll (replaces the
  //              old right-edge scrollbar). Reserving the modifier-free
  //              gesture for history makes the most common "scrub back
  //              in time" interaction the cheapest one to discover.
  // Double-click = reset to full view.
  // Click without movement (≤4 px) places f1 cursor (or f2 with Shift).
  const MIN_SPAN = 0.001;
  const DRAG_THRESHOLD = 4;
  const dragRef = useRef(null);
  // Mirror scrollOffset into a ref so the wheel handler can accumulate
  // many events per render frame without lagging behind a stale prop.
  const scrollOffsetRef = useRef(scrollOffset);
  useEffect(() => { scrollOffsetRef.current = scrollOffset; }, [scrollOffset]);
  // Live drag rectangle in canvas-pixel coords; null when not dragging.
  // Driving this through React state means the bounding box re-renders
  // on every move, but it's a single absolutely-positioned div - cheap.
  const [dragRect, setDragRect] = useState(null);

  const inSpectroOrSpectrum = useCallback((cx, cy) => {
    const lay = layoutRef.current;
    for (const k of ['spectrogram', 'spectrum']) {
      const p = lay[k];
      if (p?.visible &&
          cx >= p.x && cx < p.x + p.w &&
          cy >= p.y && cy < p.y + p.h) {
        return { panel: p, key: k };
      }
    }
    return null;
  }, []);

  const handleWheel = useCallback((e) => {
    if (!paused) return;
    const canvas = canvasRef.current;
    if (!canvas) return;
    const rect = canvas.getBoundingClientRect();
    const cx = e.clientX - rect.left;
    const cy = e.clientY - rect.top;
    const hit = inSpectroOrSpectrum(cx, cy);
    if (!hit) return;
    const { panel, key } = hit;
    e.preventDefault();

    const s = settingsRef.current;
    const factor = e.deltaY > 0 ? 1.25 : 1 / 1.25;

    // Shift = time-axis (Y) zoom on the spectrogram. Anchors at the
    // cursor's row so wheeling on a feature keeps it under the cursor
    // as the view tightens.
    if (e.shiftKey && key === 'spectrogram') {
      const span = s.viewYMax - s.viewYMin;
      const tView = (cy - panel.y) / panel.h;
      const tAxisAtCursor = s.viewYMin + tView * span;
      let newSpan = Math.max(MIN_SPAN, Math.min(1, span * factor));
      let newMin = tAxisAtCursor - tView * newSpan;
      let newMax = newMin + newSpan;
      if (newMin < 0) { newMin = 0; newMax = newSpan; }
      if (newMax > 1) { newMax = 1; newMin = 1 - newSpan; }
      onViewY?.(newMin, newMax);
      return;
    }

    // Alt = frequency-axis (X) zoom on either panel.
    if (e.altKey) {
      const span = s.viewXMax - s.viewXMin;
      const tView = (cx - panel.x) / panel.w;
      const tAxisAtCursor = s.viewXMin + tView * span;
      let newSpan = Math.max(MIN_SPAN, Math.min(1, span * factor));
      let newMin = tAxisAtCursor - tView * newSpan;
      let newMax = newMin + newSpan;
      if (newMin < 0) { newMin = 0; newMax = newSpan; }
      if (newMax > 1) { newMax = 1; newMin = 1 - newSpan; }
      onViewX?.(newMin, newMax);
      return;
    }

    // Plain wheel on the spectrogram = deep-history scroll. deltaY > 0
    // (scroll down) advances toward older rows. The spectrum panel has
    // no time axis, so plain wheel there is inert. Accumulate through
    // a ref so a burst of wheel events within one render frame doesn't
    // operate on a stale scrollOffset prop.
    if (key === 'spectrogram' && onScrollOffset) {
      const maxOffset = Math.max(0, historyCapacity - panel.h);
      const step = Math.round(e.deltaY * 0.5);
      const cur = scrollOffsetRef.current;
      const next = Math.max(0, Math.min(maxOffset, cur + step));
      if (next !== cur) {
        scrollOffsetRef.current = next;
        onScrollOffset(next);
      }
    }
  }, [paused, inSpectroOrSpectrum, onViewX, onViewY, onScrollOffset, historyCapacity]);

  const handleMouseDown = useCallback((e) => {
    if (!paused) return;
    if (e.button !== 0) return;
    const canvas = canvasRef.current;
    if (!canvas) return;
    const rect = canvas.getBoundingClientRect();
    const cx = e.clientX - rect.left;
    const cy = e.clientY - rect.top;
    const hit = inSpectroOrSpectrum(cx, cy);
    if (!hit) return;
    dragRef.current = {
      startCx: cx, startCy: cy,
      panel: hit.panel, key: hit.key,
      startXMin: settingsRef.current.viewXMin,
      startXMax: settingsRef.current.viewXMax,
      startYMin: settingsRef.current.viewYMin,
      startYMax: settingsRef.current.viewYMax,
      moved: false,
    };
  }, [paused, inSpectroOrSpectrum]);

  const handleMouseMove = useCallback((e) => {
    const d = dragRef.current;
    if (!d) return;
    const canvas = canvasRef.current;
    if (!canvas) return;
    const rect = canvas.getBoundingClientRect();
    const cx = e.clientX - rect.left;
    const cy = e.clientY - rect.top;
    const dx = cx - d.startCx;
    const dy = cy - d.startCy;
    if (!d.moved && (dx*dx + dy*dy) < DRAG_THRESHOLD * DRAG_THRESHOLD) return;
    d.moved = true;

    // Spectrogram drags are 2D rectangles (X + time). Spectrum panel
    // drags stay 1D - the y axis on spectrum is dB and we don't zoom
    // there.
    const x0 = Math.min(cx, d.startCx);
    const x1 = Math.max(cx, d.startCx);
    const y0 = (d.key === 'spectrogram') ? Math.min(cy, d.startCy) : d.panel.y;
    const y1 = (d.key === 'spectrogram') ? Math.max(cy, d.startCy)
                                         : d.panel.y + d.panel.h;
    setDragRect({
      x: Math.max(d.panel.x, x0),
      y: Math.max(d.panel.y, y0),
      w: Math.min(d.panel.x + d.panel.w, x1) - Math.max(d.panel.x, x0),
      h: Math.min(d.panel.y + d.panel.h, y1) - Math.max(d.panel.y, y0),
    });
  }, []);

  const handleMouseUp = useCallback((e) => {
    const d = dragRef.current;
    dragRef.current = null;
    setDragRect(null);
    if (!d) return;

    if (!d.moved) {
      // No drag - treat as cursor placement.
      handleClick(e);
      return;
    }

    // Apply marquee zoom. Convert drag's edges to axis-space using the
    // viewport that was active WHEN the drag began. X applies to both
    // panels; Y only applies to the spectrogram.
    const canvas = canvasRef.current;
    if (!canvas) return;
    const rect = canvas.getBoundingClientRect();
    const endCx = e.clientX - rect.left;
    const endCy = e.clientY - rect.top;

    // X range
    const x0 = Math.max(d.panel.x, Math.min(endCx, d.startCx));
    const x1 = Math.min(d.panel.x + d.panel.w, Math.max(endCx, d.startCx));
    if (x1 - x0 >= DRAG_THRESHOLD) {
      const xspan = d.startXMax - d.startXMin;
      const tViewLo = (x0 - d.panel.x) / d.panel.w;
      const tViewHi = (x1 - d.panel.x) / d.panel.w;
      let newMin = d.startXMin + tViewLo * xspan;
      let newMax = d.startXMin + tViewHi * xspan;
      if (newMax - newMin < MIN_SPAN) {
        const c = (newMin + newMax) / 2;
        newMin = Math.max(0, c - MIN_SPAN / 2);
        newMax = Math.min(1, c + MIN_SPAN / 2);
      }
      onViewX?.(newMin, newMax);
    }

    // Y range - spectrogram only. The y axis is "time elapsed since
    // newest row," so y=0 is now and y=1 is the oldest visible row.
    if (d.key === 'spectrogram') {
      const y0 = Math.max(d.panel.y, Math.min(endCy, d.startCy));
      const y1 = Math.min(d.panel.y + d.panel.h, Math.max(endCy, d.startCy));
      if (y1 - y0 >= DRAG_THRESHOLD) {
        const yspan = d.startYMax - d.startYMin;
        const tViewLo = (y0 - d.panel.y) / d.panel.h;
        const tViewHi = (y1 - d.panel.y) / d.panel.h;
        let newMin = d.startYMin + tViewLo * yspan;
        let newMax = d.startYMin + tViewHi * yspan;
        if (newMax - newMin < MIN_SPAN) {
          const c = (newMin + newMax) / 2;
          newMin = Math.max(0, c - MIN_SPAN / 2);
          newMax = Math.min(1, c + MIN_SPAN / 2);
        }
        onViewY?.(newMin, newMax);
      }
    }
  }, [handleClick, onViewX, onViewY]);

  const handleDoubleClick = useCallback((e) => {
    if (!paused) return;
    const canvas = canvasRef.current;
    if (!canvas) return;
    const rect = canvas.getBoundingClientRect();
    const cx = e.clientX - rect.left;
    const cy = e.clientY - rect.top;
    if (!inSpectroOrSpectrum(cx, cy)) return;
    onViewX?.(0, 1); // reset to full view
    onViewY?.(0, 1);
  }, [paused, inSpectroOrSpectrum, onViewX, onViewY]);

  // Wheel listener has to be { passive: false } so we can preventDefault;
  // React's synthetic onWheel is passive by default, so attach manually.
  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    canvas.addEventListener('wheel', handleWheel, { passive: false });
    return () => canvas.removeEventListener('wheel', handleWheel);
  }, [handleWheel]);

  // Drag-handle: reuses the same onDiv callback from usePanelLayout.
  // For absolute positioning we wire the mousedown directly.
  const handleDragStart = useCallback((kA, kB, e) => {
    onDiv?.(kA, kB, e);
  }, [onDiv]);

  return (
    <div ref={_cRef} style={{ flex: 1, position: 'relative', overflow: 'hidden', minWidth: 0 }}>
      {/* The single rendering canvas - fills the container */}
      <canvas
        ref={canvasRef}
        onClick={paused ? undefined : handleClick}
        onMouseDown={paused ? handleMouseDown : undefined}
        onMouseMove={paused ? handleMouseMove : undefined}
        onMouseUp={paused ? handleMouseUp : undefined}
        onMouseLeave={paused ? handleMouseUp : undefined}
        onDoubleClick={paused ? handleDoubleClick : undefined}
        style={{
          position: 'absolute', inset: 0,
          width: '100%', height: '100%',
          display: 'block',
          cursor: paused ? 'crosshair' : 'crosshair',
        }}
      />

      {/* Marquee zoom rectangle - visible only during an active drag */}
      {dragRect && (
        <div
          style={{
            position: 'absolute',
            left: dragRect.x,
            top: dragRect.y,
            width: dragRect.w,
            height: dragRect.h,
            background: T.primary + '22',
            border: `1px solid ${T.primary}`,
            pointerEvents: 'none',
            zIndex: 4,
          }}
        />
      )}

      {/* Hidden panels are managed via the control-strip toggles - no
          in-canvas bar, no real estate consumed. */}

      {/* Visible panel overlays (drag handle + header) */}
      {visKeys.map((k, i) => {
        const rect = layout[k];
        if (!rect || !rect.visible) return null;
        const prevKey = i > 0 ? visKeys[i - 1] : null;
        return (
          <PanelOverlay
            key={k}
            panelKey={k}
            rect={rect}
            title={panelTitles?.[k] ?? PANEL_LABELS[k]}
            toolbar={panelToolbars?.[k]}
            onToggle={() => tog(k)}
            onDragStart={prevKey ? (e) => handleDragStart(prevKey, k, e) : null}
          />
        );
      })}

      {/* Spectrogram bottom-ruler labels (DOM, not canvas). The dark
          strip is still painted by the GPU pass; only labels move to
          React so they pick up IBM Plex + native antialiasing. */}
      {layout.spectrogram?.visible && (
        <SpectroRulerLabels
          rect={layout.spectrogram}
          sampleRate={sampleRate}
          centerFreq={centerFreq}
          twoSided={spectrumTwoSided}
          scale={s.freqScale}
          xMin={viewXMin}
          xMax={viewXMax}
        />
      )}
      {/* f1 / f2 cursors (DOM, not canvas). Cursors update only on
          click, so CSS dashed borders + a translucent band beat
          re-uploading line geometry every frame. */}
      {layout.spectrogram?.visible && (
        <CursorOverlay
          rect={layout.spectrogram}
          panel="spectrogram"
          sampleRate={sampleRate}
          centerFreq={centerFreq}
          twoSided={spectrumTwoSided}
          scale={s.freqScale}
          xMin={viewXMin}
          xMax={viewXMax}
          f1={cursorF1}
          f2={cursorF2}
        />
      )}
      {layout.spectrum?.visible
       && activeTab !== 'histogram'
       && activeTab !== 'iq'
       && activeTab !== 'waveform' && (
        <CursorOverlay
          rect={layout.spectrum}
          panel="spectrum"
          sampleRate={sampleRate}
          centerFreq={centerFreq}
          twoSided={spectrumTwoSided}
          scale={s.freqScale}
          xMin={viewXMin}
          xMax={viewXMax}
          f1={cursorF1}
          f2={cursorF2}
        />
      )}
    </div>
  );
}

// Pink/purple dashed verticals at f1/f2 plus a translucent band
// between them. Re-renders only on rect / f1 / f2 change — the GPU
// RAF loop is no longer involved in cursor rendering.
//
// The plot area inside a panel is `rect` minus the same margins the GPU
// renderers use (ML/MR/MT/MB in CSS pixels), so x positions land on the
// same pixels the trace, grid, and peak markers do. The spectrogram has
// no left margin (no dB axis), matching the canvas-side `hasML` test.
const CursorOverlay = function CursorOverlay({
  rect, panel, sampleRate, centerFreq, twoSided, scale,
  xMin = 0, xMax = 1, f1, f2,
}) {
  // Margins must match the GPU renderers' plot rectangle exactly. The
  // spectrum panel uses ML/MR/MT/MB; the spectrogram has no top margin
  // (data fills the panel) and its bottom inset is the React ruler
  // strip (RULER_STRIP_PX = 22), not the frequency-label margin.
  const ML_CSS = 34, MR_CSS = 8, MT_CSS = 12, MB_CSS = 16;
  const SPECTRO_RULER_PX = 22;
  const isSpectrogram = panel === 'spectrogram';
  const plotLeft   = rect.x + (isSpectrogram ? 0 : ML_CSS);
  const plotRight  = rect.x + rect.w - MR_CSS;
  const plotTop    = rect.y + (isSpectrogram ? 0 : MT_CSS);
  const plotBottom = rect.y + rect.h - (isSpectrogram ? SPECTRO_RULER_PX : MB_CSS);
  const plotWidth  = Math.max(1, plotRight - plotLeft);
  const plotHeight = Math.max(1, plotBottom - plotTop);
  const span = xMax - xMin;

  const xFor = (f) => {
    if (f == null || !isFinite(f)) return null;
    const tAxis = freqToT(f, sampleRate, centerFreq, twoSided, scale);
    const tView = (tAxis - xMin) / span;
    if (tView < 0 || tView > 1) return null;
    return plotLeft + tView * plotWidth;
  };

  const x1 = xFor(f1);
  const x2 = xFor(f2);
  if (x1 == null && x2 == null) return null;

  // Highlight band between f1 and f2 - same translucent primary-color
  // wash the GPU path drew via C_BAND.
  const bandLeft  = (x1 != null && x2 != null) ? Math.min(x1, x2) : null;
  const bandRight = (x1 != null && x2 != null) ? Math.max(x1, x2) : null;

  return (
    <>
      {bandLeft != null && (
        <div style={{
          position: 'absolute',
          top: plotTop, left: bandLeft,
          width: bandRight - bandLeft, height: plotHeight,
          background: T.primary,
          opacity: 0.07,
          pointerEvents: 'none',
          zIndex: 2,
        }} />
      )}
      {x1 != null && (
        <CursorLine x={x1} top={plotTop} height={plotHeight} tagTop={rect.y + 2}
                    color={T.pink}   tag="f1" />
      )}
      {x2 != null && (
        <CursorLine x={x2} top={plotTop} height={plotHeight} tagTop={rect.y + 2}
                    color={T.purple} tag="f2" />
      )}
    </>
  );
};

// Single dashed vertical with a small bold tag at the panel top. The tag
// is translateX(-50%) centered on the line, matching the GPU path's
// `text.measure(tag) / 2` trick. `tagTop` is decoupled from the line's
// `top` so the tag can live in the panel's header strip (spectrum) or
// inside the top of the data (spectrogram, which has no top margin).
function CursorLine({ x, top, height, tagTop, color, tag }) {
  return (
    <>
      <div style={{
        position: 'absolute',
        top, left: x,
        width: 0, height,
        borderLeft: `1px dashed ${color}`,
        pointerEvents: 'none',
        zIndex: 3,
      }} />
      <span style={{
        position: 'absolute',
        top: tagTop, left: x,
        transform: 'translateX(-50%)',
        fontFamily: T.font,
        fontSize: 12,
        fontWeight: 600,
        color,
        whiteSpace: 'nowrap',
        pointerEvents: 'none',
        zIndex: 4,
      }}>{tag}</span>
    </>
  );
}

// 9 frequency tick labels along the bottom of the spectrogram panel.
// Memoized, re-renders only when rect or freq math params change.
const RULER_TICKS = 9;
const RULER_STRIP_PX = 22; // matches the canvas-side strip height
const SpectroRulerLabels = function SpectroRulerLabels({
  rect, sampleRate, centerFreq, twoSided, scale,
  xMin = 0, xMax = 1,
}) {
  const labels = useMemo(() => {
    const out = [];
    for (let i = 0; i < RULER_TICKS; i++) {
      const tView = i / (RULER_TICKS - 1);
      // Map viewport position -> axis position so labels reflect the
      // currently zoomed window. With no zoom xMin=0, xMax=1 -> identity.
      const tAxis = xMin + tView * (xMax - xMin);
      const f = tToFreq(tAxis, sampleRate, centerFreq, twoSided, scale);
      out.push({ t: tView, text: fmtFreqShort(f) });
    }
    return out;
  }, [sampleRate, centerFreq, twoSided, scale, xMin, xMax]);

  const top  = rect.y + rect.h - RULER_STRIP_PX + 4; // 4px padding
  const left = rect.x;
  const width = rect.w;

  return (
    <div
      style={{
        position: 'absolute',
        top, left, width, height: RULER_STRIP_PX - 4,
        pointerEvents: 'none',
        zIndex: 2,
      }}
    >
      {labels.map((lbl, i) => {
        // First and last labels would otherwise hang half off the panel
        // edge (translateX(-50%) recenters on a point that sits exactly
        // on the panel boundary). Anchor those two to the inside edge so
        // "0" / "24k" stays fully visible.
        const isFirst = i === 0;
        const isLast  = i === labels.length - 1;
        const left = isFirst ? '0%' : isLast ? '100%' : `${lbl.t * 100}%`;
        const transform = isFirst ? 'none'
                         : isLast  ? 'translateX(-100%)'
                         :           'translateX(-50%)';
        return (
          <span
            key={i}
            style={{
              position: 'absolute',
              left, transform,
              fontFamily: T.font,
              fontSize: 11,
              fontWeight: 500,
              color: T.text,
              whiteSpace: 'nowrap',
            }}
          >
            {lbl.text}
          </span>
        );
      })}
    </div>
  );
};

// Drag handle + title bar, absolutely positioned over the canvas.
// No children — the panel content is the GPU canvas underneath.
function PanelOverlay({ panelKey, rect, title, toolbar, onToggle, onDragStart }) {
  const [hDrag, setHDrag] = useState(false);

  return (
    <>
      {/* Drag handle - sits above the panel header */}
      {onDragStart && (
        <div
          onMouseDown={onDragStart}
          onMouseEnter={() => setHDrag(true)}
          onMouseLeave={() => setHDrag(false)}
          style={{
            position: 'absolute',
            top: rect.domY - DRAG_H,
            left: 0, right: 0,
            height: DRAG_H,
            cursor: 'row-resize',
            background: hDrag ? T.primaryDim + '44' : 'transparent',
            display: 'flex', alignItems: 'center', justifyContent: 'center',
            zIndex: 3,
          }}
        >
          <div style={{
            width: 30, height: 2, borderRadius: 1,
            background: hDrag ? T.primary : T.border,
          }} />
        </div>
      )}

      {/* Panel header / title bar */}
      <div
        style={{
          position: 'absolute',
          top: rect.domY + (onDragStart ? 0 : 0),
          left: 0, right: 0,
          height: HEADER_H,
          background: T.bgElev,
          borderBottom: `1px solid ${T.borderSub}`,
          display: 'flex', alignItems: 'center',
          justifyContent: 'space-between',
          padding: '0 10px',
          zIndex: 2,
          boxSizing: 'border-box',
        }}
      >
        <span style={{
          fontFamily: T.font, fontSize: 10, fontWeight: 600,
          color: T.textMuted, textTransform: 'uppercase',
          letterSpacing: '.08em', display: 'flex', alignItems: 'center', gap: 8,
        }}>
          {title}
        </span>
        <div style={{ display: 'flex', gap: 6, alignItems: 'center' }}>
          {toolbar}
          <button
            onClick={onToggle}
            style={{
              background: 'none', border: 'none', color: T.textMuted,
              cursor: 'pointer', fontFamily: T.font, fontSize: 10,
              padding: '0 4px', lineHeight: 1,
            }}
          >
            ✕
          </button>
        </div>
      </div>
    </>
  );
}
