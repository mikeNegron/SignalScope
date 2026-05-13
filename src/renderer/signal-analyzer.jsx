import { useState, useEffect, useRef, useCallback, useMemo } from "react";
import { T } from "./lib/tokens.js";
import { useSimulationLoop } from "./hooks/use-simulation-loop.js";
import { usePanelLayout } from "./hooks/use-panel-layout.js";
import { useMeasurements } from "./hooks/use-measurements.js";
import { usePersistedState } from "./hooks/use-persisted-state.js";
import { TitleBar } from "./components/title-bar.jsx";
import { TopBar } from "./components/top-bar.jsx";
import { ControlStrip } from "./components/control-strip.jsx";
import { StatusBar } from "./components/status-bar.jsx";
import { Sidebar } from "./components/sidebar.jsx";
import { CursorInfoBar } from "./components/cursor-bar.jsx";
import { DragHandle } from "./components/panels.jsx";
import { RenderSurface } from "./components/RenderSurface.jsx";
import { FileLoadModal } from "./components/FileLoadModal.jsx";

export default function SignalAnalyzer({
  simulationEnabled = true,
  backendConnected = false,
  wsDataRef = null,      // raw buf ref from useBackendConnection - GPU reads directly
  backendFrameId = 0,
  backendClipping = false,
  backendSampleRate = 48000,
  backendLoopCount = 0,
  backendSourceMode = 0,         // 0=test, 1=mic, 2=file
  backendSourceComplex = false,  // true ONLY when source itself is IQ (file/SDR)
  backendSpectrumTwoSided = false, // true when FFT span is -fs/2..+fs/2 (complex source OR DDC tune≠0 OR Hilbert)
  backendCenterFreq = 0,         // Hz; non-zero only for SDR/SigMF captures
  onBackendCommand = null,
  onReconnect = null,
  onPause = null,
}) {
  // UI state. usePersistedState mirrors to localStorage; plain useState
  // for things that shouldn't survive reload (source mode, pause,
  // cursors, view zoom).
  const [mode, setMode] = useState("play");
  const [fftSize, setFftSize] = usePersistedState("fftSize", "4096");
  const [windowFn, setWindowFn] = usePersistedState("windowFn", "hanning");
  const [colorMap, setColorMap] = usePersistedState("colorMap", "baudline");
  const [gain, setGain] = usePersistedState("gain", 0);
  const [avgCount, setAvgCount] = usePersistedState("avgCount", 1);
  const [showPeaks, setShowPeaks] = usePersistedState("showPeaks", true);
  const [periodicityBars, setPeriodicityBars] = usePersistedState("periodicityBars", false);
  const [sampleRate, setSampleRate] = useState(48000);
  const [activeTab, setActiveTab] = usePersistedState("activeTab", "spectrum");
  const [sidebarTab, setSidebarTab] = usePersistedState("sidebarTab", "measure");
  const [cursorF1, setCursorF1] = useState(null);
  const [cursorF2, setCursorF2] = useState(null);
  const [brightness, setBrightness] = usePersistedState("brightness", 50);
  const [contrast, setContrast] = usePersistedState("contrast", 70);
  const [scrollSpeed, setScrollSpeed] = usePersistedState("scrollSpeed", 3);
  const [paused, setPaused] = useState(false);
  const [peakHold, setPeakHold] = usePersistedState("peakHold", false);
  const [freqScale, setFreqScale] = usePersistedState("freqScale", "linear");
  const [tuneHz, setTuneHz] = usePersistedState("tuneHz", 0);
  const [decimation, setDecimation] = usePersistedState("decimation", 1);
  const [suppressImage, setSuppressImage] = usePersistedState("suppressImage", false);
  const [peakMinLevel, setPeakMinLevel] = usePersistedState("peakMinLevel", -60);
  const [weighting, setWeighting] = usePersistedState("weighting", "none");
  const [overlap, setOverlap] = usePersistedState("overlap", 0);
  // File replay speed multiplier. 1× is real-time playback; higher
  // values play the file faster than recorded, lifting the FFT-rate
  // ceiling at large fft_size (samples arrive faster than 1× wall-clock).
  const [replaySpeed, setReplaySpeed] = usePersistedState("replaySpeed", 1.0);
  const [taperCount, setTaperCount] = usePersistedState("taperCount", 1);
  // Deep-history scroll. Transient - resets when paused state ends.
  const [scrollOffset, setScrollOffset] = useState(0);
  const [historyChunk, setHistoryChunk] = usePersistedState("historyChunk", 512);
  const [historyCapacity, setHistoryCapacity] = usePersistedState("historyCapacity", 4096);
  // Listen-source state - persisted (port, format, defaults).
  const [listenPort,         setListenPort]         = usePersistedState("listenPort",         5555);
  const [listenFormat,       setListenFormat]       = usePersistedState("listenFormat",       2);
  const [listenSampleRate,   setListenSampleRate]   = usePersistedState("listenSampleRate",   48000);
  const [listenCenterFreq,   setListenCenterFreq]   = usePersistedState("listenCenterFreq",   0);
  const [listenExpectHeader, setListenExpectHeader] = usePersistedState("listenExpectHeader", true);
  // Tracks the source picked from the Input tab. Persisted so the
  // selection survives sidebar-tab unmount/remount (InputTab tears
  // down when the user navigates away) and full page reloads.
  // Includes the UI-only labels "websocket" and "simulated" — those
  // don't trigger a backend set_source command, but the dropdown
  // still needs to reflect the last selection.
  const [currentSource, setCurrentSource] = usePersistedState("currentSource", "websocket");
  // Spectrogram/spectrum X-axis viewport in normalized [0,1] coords. Zoom
  // controls (wheel/drag) only respond when paused; on unpause we snap
  // back to full-axis. xMin < xMax is invariant; minimum span 0.001.
  const [viewXMin, setViewXMin] = useState(0);
  const [viewXMax, setViewXMax] = useState(1);
  // Time-axis (y) viewport on the spectrogram. 0..1 = full visible
  // history; narrowing it engages auto-overlap (see effect below).
  const [viewYMin, setViewYMin] = useState(0);
  const [viewYMax, setViewYMax] = useState(1);
  const resetView = useCallback(() => {
    setViewXMin(0); setViewXMax(1);
    setViewYMin(0); setViewYMax(1);
  }, []);
  // Snap viewport back to full when leaving paused state.
  useEffect(() => {
    if (!paused) resetView();
  }, [paused, resetView]);

  // File-load modal state. When the user picks a non-self-describing file
  // (raw / .f32 / sigmf-data without sidecar), we open this modal to collect
  // the missing parameters before sending load_file.
  const [fileModal, setFileModal] = useState(null); // { path, format, hint } | null

  // backendConnected flips true only after the first data packet,
  // so it doubles as "has live data" without a separate flag.
  const backendActive = backendConnected;

  const { simDataRef, timeRef } =
    useSimulationLoop({ paused, fftSize, backendActive, windowFn, gain, simulationEnabled });

  // Single ref the GPU RAF loop reads each frame - zero React re-renders from data.
  // Points to the live backend buffer or the simulation buffer depending on mode.
  const displayDataRef = useRef(null);
  displayDataRef.current = backendActive
    ? (wsDataRef?.current ?? null)
    : (simulationEnabled ? simDataRef.current : null);

  // Snapshot spectrum + waveform at ~4fps for sidebar and measurements.
  // Backend buf arrays are reused in-place (.set()), so we can't rely on reference
  // identity - instead check snapSpec/snapWav dirty flags set by onmessage.
  // Simulation data is still new-array-per-frame, so reference change works there.
  // Either way we copy on snapshot so useMemo in useMeasurements sees a new ref.
  const [snapSpectrum, setSnapSpectrum] = useState(null);
  const [snapWaveform, setSnapWaveform] = useState(null);
  // SpectrumStats snapshot - peaks/noiseFloor/fundamentalFreq from the backend.
  // Refreshed on the same 4Hz cadence so React re-renders are amortized.
  const [snapStats, setSnapStats] = useState(null);
  const lastSeqRef = useRef({ spec: -1, wav: -1, stats: -1 });
  useEffect(() => {
    const id = setInterval(() => {
      const d = displayDataRef.current;
      if (!d) return;
      if (d.spectrum && d.specSeq !== lastSeqRef.current.spec) {
        lastSeqRef.current.spec = d.specSeq;
        setSnapSpectrum(new Float32Array(d.spectrum));
      }
      if (d.waveform && d.wavSeq !== lastSeqRef.current.wav) {
        lastSeqRef.current.wav = d.wavSeq;
        setSnapWaveform(new Float32Array(d.waveform));
      }
      // Stats: shallow-copy the peaks array so the consumer's useMemo sees a
      // fresh reference; the underlying objects are immutable enough for our
      // purposes (binIdx/value/prom/N are read-only after parsing).
      if (d.statsSeq != null && d.statsSeq !== lastSeqRef.current.stats) {
        lastSeqRef.current.stats = d.statsSeq;
        setSnapStats({
          peaks: d.peaks.slice(),
          noiseFloor: d.noiseFloor,
          fundamentalFreq: d.fundamentalFreq,
        });
      }
    }, 250);
    return () => clearInterval(id);
  }, []);

  useEffect(() => {
    if (backendConnected && backendSampleRate) setSampleRate(backendSampleRate);
  }, [backendConnected, backendSampleRate]);

  const { panels, tog, pFracs, containerRef, onDiv, sbW, onSbDrag, visKeys, hidKeys } =
    usePanelLayout();

  const meas = useMeasurements(snapStats, sampleRate, backendCenterFreq, backendSourceComplex);

  const cmd = onBackendCommand;

  // Time-axis zoom-escalated overlap. When the user zooms in on the
  // spectrogram's y axis (paused mode), more FFTs per second are needed
  // so each visible row stays one FFT computation. Math: at full view
  // the backend's hop is `fft_size / overlap_factor` and the visible
  // window is panel_h × hop / fs. Zooming to a y-span of yz means we
  // want visible_window × yz seconds across the same panel_h rows:
  //   new_hop = old_hop × yz   ->   new_factor = old_factor / yz.
  // Snap up to the nearest supported factor in {1, 2, 4, 8, 16}.
  // On unzoom/unpause the viewport snaps back and we revert to the
  // user's static `overlap` setting.
  useEffect(() => {
    if (!cmd) return;
    const yspan = viewYMax - viewYMin;
    if (yspan >= 0.999) {
      cmd("set_overlap", overlap);
      return;
    }
    const baseFactor = (overlap >= 75) ? 4 : (overlap >= 50) ? 2 : 1;
    const target = Math.ceil(baseFactor / yspan);
    let pct = 0;
    if      (target >= 16) pct = 93;
    else if (target >= 8)  pct = 87;
    else if (target >= 4)  pct = 75;
    else if (target >= 2)  pct = 50;
    cmd("set_overlap", pct);
  }, [cmd, overlap, viewYMin, viewYMax]);
  // Sync persisted settings to the backend whenever a connection
  // becomes available. usePersistedState restores user choices on
  // mount, but those values never went through the `set_*` callbacks
  // so the backend doesn't know about them until we push here. Gated
  // on backendConnected so the effect fires AFTER the WebSocket
  // actually opens — sendCommand silently drops messages while
  // readyState !== OPEN, so depending on cmd alone (which is
  // identity-stable from first render) means the sync runs against
  // a closed socket and the backend never hears about the user's
  // saved choices. Reset on disconnect so reconnect re-syncs.
  const syncedRef = useRef(false);
  useEffect(() => {
    if (!cmd || !backendConnected) { syncedRef.current = false; return; }
    if (syncedRef.current) return;
    syncedRef.current = true;
    cmd("set_fft_size",       +fftSize);
    cmd("set_window",          windowFn);
    cmd("set_gain",            gain);
    cmd("set_avg_count",       avgCount);
    cmd("set_tune_offset",     tuneHz);
    cmd("set_decimation",      decimation);
    cmd("set_suppress_image",  suppressImage);
    cmd("set_peak_min_level",  peakMinLevel);
    cmd("set_weighting",       weighting);
    cmd("set_overlap",         overlap);
    cmd("set_replay_speed",    replaySpeed);
    cmd("set_taper_count",     taperCount);
    cmd("set_history_capacity", historyCapacity);
    cmd("set_listen_port",          listenPort);
    cmd("set_listen_format",        listenFormat);
    cmd("set_listen_sample_rate",   listenSampleRate);
    cmd("set_listen_center_freq",   listenCenterFreq);
    cmd("set_listen_expect_header", listenExpectHeader);
    // Restore the source if it's a real backend mode. "websocket" and
    // "simulated" are UI-only labels (no command); "file" requires a
    // path the user has to re-pick anyway, so we skip it too.
    if (currentSource !== "websocket"
        && currentSource !== "simulated"
        && currentSource !== "file") {
      cmd("set_source", currentSource);
    }
    // Note: NOT pushing sampleRate - backend tracks its own source
    // rate and pushes it back via SourceStatus; pushing here would
    // race with that.
  }, [cmd, backendConnected]);

  const hFFT  = useCallback((v) => { setFftSize(v);  cmd?.("set_fft_size",  +v); }, [cmd]);
  const hWin  = useCallback((v) => { setWindowFn(v); cmd?.("set_window",    v);  }, [cmd]);
  const hGain = useCallback((v) => { setGain(v);     cmd?.("set_gain",      v);  }, [cmd]);
  const hAvg  = useCallback((v) => { setAvgCount(v); cmd?.("set_avg_count", v); }, [cmd]);
  const hSR   = useCallback((v) => { setSampleRate(+v); cmd?.("set_sample_rate", +v); }, [cmd]);
  const hTune = useCallback((v) => { setTuneHz(+v); cmd?.("set_tune_offset", +v); }, [cmd]);
  const hDecim = useCallback((v) => {
    const n = Math.max(1, Math.round(+v));
    setDecimation(n);
    cmd?.("set_decimation", n);
  }, [cmd]);
  const hSuppressImage = useCallback(() => {
    setSuppressImage((on) => {
      const next = !on;
      cmd?.("set_suppress_image", next);
      return next;
    });
  }, [cmd]);
  const hPeakMinLevel = useCallback((v) => {
    const n = Math.max(-130, Math.min(0, +v));
    setPeakMinLevel(n);
    cmd?.("set_peak_min_level", n);
  }, [cmd]);
  const hWeighting = useCallback((v) => {
    setWeighting(v);
    cmd?.("set_weighting", v);
  }, [cmd]);
  const hOverlap = useCallback((v) => {
    const pct = (v >= 75) ? 75 : (v >= 50) ? 50 : 0;
    setOverlap(pct);
    cmd?.("set_overlap", pct);
  }, [cmd]);
  const hReplaySpeed = useCallback((v) => {
    const n = Math.max(0.1, Math.min(32, +v || 1.0));
    setReplaySpeed(n);
    cmd?.("set_replay_speed", n);
  }, [cmd]);
  const hTaperCount = useCallback((v) => {
    const k = (v >= 8) ? 8 : (v >= 4) ? 4 : (v >= 2) ? 2 : 1;
    setTaperCount(k);
    cmd?.("set_taper_count", k);
  }, [cmd]);
  const hHistoryChunk = useCallback((v) => {
    const n = Math.max(64, Math.min(8192, Math.round(+v)));
    setHistoryChunk(n);
  }, []);
  const hHistoryCapacity = useCallback((v) => {
    const n = Math.max(256, Math.min(65536, Math.round(+v)));
    setHistoryCapacity(n);
    cmd?.("set_history_capacity", n);
  }, [cmd]);
  // Push the initial capacity once the backend is reachable.
  useEffect(() => {
    if (cmd) cmd("set_history_capacity", historyCapacity);
  }, [cmd, historyCapacity]);
  // When pause is released or scroll snaps back to 0, drop any
  // accumulated history rows so the live texture resumes clean.
  useEffect(() => {
    if (!paused) {
      if (scrollOffset !== 0) setScrollOffset(0);
      const buf = displayDataRef.current;
      if (buf?._history) buf._history.rows = {};
    }
  }, [paused, scrollOffset]);
  // Debounced history request whenever scrollOffset changes while
  // paused. Chunk size determines how many extra rows we fetch
  // beyond the visible window so small pans don't trigger another
  // round-trip.
  useEffect(() => {
    if (!cmd || !paused || scrollOffset <= 0) return;
    const visible = 800; // approximate; backend serves what's available
    const handle = setTimeout(() => {
      // Drop the prior batch so the response *replaces* the displayed
      // rows instead of unioning with them. Without this, rows from
      // earlier scroll positions linger in hist.rows{} and the renderer
      // (which uploads every row it finds) ends up anchoring the top
      // of the spectrogram to the smallest cached offset - making the
      // view look stuck on the first scroll position.
      const buf = displayDataRef.current;
      if (buf?._history) {
        buf._history.rows = {};
      }
      cmd("request_history", { start: scrollOffset,
                                count: visible + historyChunk });
    }, 40);
    return () => clearTimeout(handle);
  }, [cmd, paused, scrollOffset, historyChunk]);
  // Backend-driven export. The renderer's job is just to ask the OS for
  // a destination path; the backend then writes its current spectrum or
  // waveform buffer directly. Matches the Discord/Figma model - bytes
  // come off the wire once, not twice.
  const onExport = useCallback(async (format, source) => {
    if (!cmd || !window.signalscope?.saveFile) return;
    const ext = { csv: "csv", raw: "f32", wav: "wav", aiff: "aiff" }[format];
    if (!ext) return;
    const path = await window.signalscope.saveFile({
      title: `Save ${source} as ${format.toUpperCase()}`,
      defaultPath: `signalscope_${Date.now()}.${ext}`,
      filters: [{ name: format.toUpperCase(), extensions: [ext] }],
    });
    if (!path) return;
    cmd("export_capture", { format, source, path });
  }, [cmd]);
  const hListenPort = useCallback((v) => {
    const p = Math.max(1, Math.min(65535, Math.round(+v)));
    setListenPort(p);
    cmd?.("set_listen_port", p);
  }, [cmd]);
  const hListenFormat = useCallback((v) => {
    const f = Math.max(0, Math.min(3, +v));
    setListenFormat(f);
    cmd?.("set_listen_format", f);
  }, [cmd]);
  const hListenSampleRate = useCallback((v) => {
    const sr = Math.max(1, Math.round(+v));
    setListenSampleRate(sr);
    cmd?.("set_listen_sample_rate", sr);
  }, [cmd]);
  const hListenCenterFreq = useCallback((v) => {
    const cf = Math.max(0, Math.round(+v));
    setListenCenterFreq(cf);
    cmd?.("set_listen_center_freq", cf);
  }, [cmd]);
  const hListenExpectHeader = useCallback((v) => {
    setListenExpectHeader(!!v);
    cmd?.("set_listen_expect_header", !!v);
  }, [cmd]);
  const hSrc  = useCallback((v) => {
    setCurrentSource(v);
    // UI-only labels: "simulated" toggles a renderer-side generator
    // upstream, "websocket" just means "use whatever the backend is
    // doing", "file" goes through the Browse → load_file path. None
    // of those need a set_source command.
    if (v === "simulated" || v === "websocket" || v === "file") return;
    cmd?.("set_source", v);
  }, [cmd]);
  // Send a fully-resolved load_file command. The backend's JSON helpers are
  // flat-key only, so the payload is sent as a flat object - sendCommand spreads
  // it directly into the message envelope.
  const sendLoadFile = useCallback((req) => {
    cmd?.("set_source", "file");
    cmd?.("load_file", {
      path: req.path,
      format: req.format,
      datatype: req.datatype || "",
      sample_rate: Number(req.sample_rate) || 0,
      channels: Number(req.channels) || 1,
      // 0 for non-IQ files; SigMF captures supply core:frequency.
      center_frequency: Number(req.center_frequency) || 0,
    });
  }, [cmd]);

  const hPeakHold = useCallback(() => {
    setPeakHold((v) => {
      const next = !v;
      cmd?.("set_peak_hold", next);
      return next;
    });
  }, [cmd]);
  const hClearPeakHold = useCallback(() => {
    cmd?.("clear_peak_hold");
  }, [cmd]);

  const hOpenFile = useCallback(async () => {
    const result = await window.signalscope?.openFile?.();
    if (!result) return;
    if (result.needsParams) {
      setFileModal({ path: result.path, format: result.format, hint: result.hint });
    } else {
      sendLoadFile({
        path: result.path,
        format: result.format,
        ...(result.hint || {}),
      });
    }
  }, [sendLoadFile]);

  const onFileModalSubmit = useCallback((params) => {
    if (!fileModal) return;
    sendLoadFile({
      path: fileModal.path,
      format: fileModal.format,
      datatype: params.datatype,
      sample_rate: params.sample_rate,
      channels: params.channels,
      center_frequency:
        Number(params.center_frequency) ||
        Number(fileModal.hint?.center_frequency) || 0,
    });
    setFileModal(null);
  }, [fileModal, sendLoadFile]);
  const hCursor = useCallback((freq, which) => {
    which === "f1" ? setCursorF1(freq) : setCursorF2(freq);
  }, []);
  const clrCursors = useCallback(() => { setCursorF1(null); setCursorF2(null); }, []);
  const hMode = useCallback((m) => {
    setMode(m);
    const shouldPause = m === "pause";
    setPaused(shouldPause);
    onPause?.(shouldPause);
    cmd?.("set_pause", shouldPause);
  }, [cmd, onPause]);

  // Spacebar = toggle pause/play. Suppressed when the user is typing into
  // an input/textarea/contentEditable so the NumInput fields, file-load
  // modal, etc. all behave normally.
  useEffect(() => {
    const onKey = (e) => {
      if (e.code !== "Space") return;
      const t = e.target;
      const tag = t?.tagName;
      if (tag === "INPUT" || tag === "TEXTAREA" || tag === "SELECT"
          || t?.isContentEditable) {
        return;
      }
      e.preventDefault();
      hMode(mode === "pause" ? "play" : "pause");
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [mode, hMode]);

  const srcLabel  = backendActive ? "C++ Backend" : "Simulated";
  const connColor = backendConnected ? T.success : T.error;

  // Panel title / toolbar JSX passed to RenderSurface
  // Memoized so frame-rate React renders (e.g., backend status flushes) don't
  // rebuild this JSX tree every render - RenderSurface gets stable prop refs.
  const panelTitles = useMemo(() => ({
    spectrogram: "Spectrogram - Waterfall",
    spectrum: (
      <div style={{ display: "flex", gap: 12 }}>
        {["spectrum", "histogram", "phase"].map((t) => (
          <span
            key={t}
            onClick={(e) => { e.stopPropagation(); setActiveTab(t); }}
            style={{
              cursor: "pointer",
              color: activeTab === t ? T.primary : T.textMuted,
              textTransform: "uppercase",
              fontSize: 10,
              fontWeight: activeTab === t ? 700 : 500,
              borderBottom: activeTab === t ? `1px solid ${T.primary}` : "none",
              paddingBottom: 2,
            }}
          >
            {t}
          </span>
        ))}
      </div>
    ),
    waveform: "Waveform - Oscilloscope",
    iq: "IQ Constellation",
  }), [activeTab]);

  const panelToolbars = useMemo(() => ({
    spectrogram: (
      <span style={{ fontFamily: T.font, fontSize: 9, color: T.textMuted }}>
        Click: f1 · Shift+Click: f2
      </span>
    ),
    spectrum: (
      <span style={{ fontFamily: T.font, fontSize: 9, color: T.textMuted }}>
        {activeTab === "spectrum" ? "dBFS / Hz" : activeTab === "phase" ? "rad / Hz" : "count"}
      </span>
    ),
    waveform: (
      <span style={{ fontFamily: T.font, fontSize: 9, color: T.textMuted }}>
        {((512 / sampleRate) * 1000).toFixed(2)} ms
      </span>
    ),
    iq: (
      <span style={{ fontFamily: T.font, fontSize: 9, color: T.textMuted }}>I/Q</span>
    ),
  }), [activeTab, sampleRate]);

  return (
    <div
      style={{
        width: "100%",
        height: "100vh",
        background: T.bg,
        color: T.text,
        fontFamily: T.fontSans,
        display: "flex",
        flexDirection: "column",
        overflow: "hidden",
        userSelect: "none",
      }}
    >
      <TitleBar />
      <TopBar
        mode={mode}
        onMode={hMode}
        backendConnected={backendConnected}
        sampleRate={sampleRate}
        fftSize={fftSize}
        srcLabel={srcLabel}
        connColor={connColor}
      />
      <ControlStrip
        fftSize={fftSize}
        windowFn={windowFn}
        colorMap={colorMap}
        gain={gain}
        avgCount={avgCount}
        showPeaks={showPeaks}
        periodicityBars={periodicityBars}
        peakHold={peakHold}
        cursorF1={cursorF1}
        cursorF2={cursorF2}
        panels={panels}
        onFFT={hFFT}
        onWin={hWin}
        onColorMap={setColorMap}
        onGain={hGain}
        onAvg={hAvg}
        onShowPeaks={() => setShowPeaks((v) => !v)}
        onPeriodicityBars={() => setPeriodicityBars((v) => !v)}
        onPeakHold={hPeakHold}
        onClearPeakHold={hClearPeakHold}
        onClearF1={() => setCursorF1(null)}
        onClearF2={() => setCursorF2(null)}
        onClearCursors={clrCursors}
        onTogglePanel={tog}
      />
      <CursorInfoBar
        cursorF1={cursorF1}
        cursorF2={cursorF2}
        sampleRate={sampleRate}
        fftSize={fftSize}
        centerFreq={backendCenterFreq}
        spectrumData={snapSpectrum}
        twoSided={backendSpectrumTwoSided}
      />
      <div style={{ flex: 1, display: "flex", overflow: "hidden" }}>
        <RenderSurface
          containerRef={containerRef}
          panels={panels}
          pFracs={pFracs}
          tog={tog}
          onDiv={onDiv}
          panelTitles={panelTitles}
          panelToolbars={panelToolbars}
          dataRef={displayDataRef}
          sampleRate={sampleRate}
          centerFreq={backendCenterFreq}
          spectrumTwoSided={backendSpectrumTwoSided}
          colorMap={colorMap}
          brightness={brightness}
          contrast={contrast}
          scrollSpeed={scrollSpeed}
          showPeaks={showPeaks}
          peakHold={peakHold}
          periodicityBars={periodicityBars}
          fundamentalFreq={meas?.fundamental || 0}
          freqScale={freqScale}
          weighting={weighting}
          paused={paused}
          viewXMin={viewXMin}
          viewXMax={viewXMax}
          onViewX={(min, max) => { setViewXMin(min); setViewXMax(max); }}
          viewYMin={viewYMin}
          viewYMax={viewYMax}
          onViewY={(min, max) => { setViewYMin(min); setViewYMax(max); }}
          scrollOffset={scrollOffset}
          onScrollOffset={setScrollOffset}
          historyCapacity={historyCapacity}
          cursorF1={cursorF1}
          cursorF2={cursorF2}
          activeTab={activeTab}
          onCursorMove={hCursor}
        />
        {panels.sidebar && <DragHandle onMouseDown={onSbDrag} direction="horizontal" />}
        {panels.sidebar && (
          <Sidebar
            sidebarTab={sidebarTab}
            onSidebarTab={setSidebarTab}
            sbW={sbW}
            meas={meas}
            backendClipping={backendClipping}
            backendConnected={backendConnected}
            backendFrameId={backendFrameId}
            timeRef={timeRef}
            srcLabel={srcLabel}
            cursorF1={cursorF1}
            cursorF2={cursorF2}
            spectrumData={snapSpectrum}
            sampleRate={sampleRate}
            fftSize={fftSize}
            onClearF1={() => setCursorF1(null)}
            onClearF2={() => setCursorF2(null)}
            onClearCursors={clrCursors}
            onSource={hSrc}
            onOpenFile={hOpenFile}
            onSampleRate={hSR}
            onReconnect={onReconnect}
            connColor={connColor}
            source={currentSource}
            replaySpeed={replaySpeed}
            onReplaySpeed={hReplaySpeed}
            listenPort={listenPort}
            listenFormat={listenFormat}
            listenSampleRate={listenSampleRate}
            listenCenterFreq={listenCenterFreq}
            listenExpectHeader={listenExpectHeader}
            listenStatus={
              currentSource !== "listen"     ? "stopped"    :
              backendSourceMode === 3        ? "connected"  :
                                               "listening"
            }
            onListenPort={hListenPort}
            onListenFormat={hListenFormat}
            onListenSampleRate={hListenSampleRate}
            onListenCenterFreq={hListenCenterFreq}
            onListenExpectHeader={hListenExpectHeader}
            brightness={brightness}
            contrast={contrast}
            scrollSpeed={scrollSpeed}
            onBrightness={setBrightness}
            onContrast={setContrast}
            onScrollSpeed={setScrollSpeed}
            waveformData={snapWaveform}
            freqScale={freqScale}
            onFreqScale={setFreqScale}
            freqScaleDisabled={backendSpectrumTwoSided}
            sourceComplex={backendSourceComplex}
            tuneHz={tuneHz}
            decimation={decimation}
            onTune={hTune}
            onDecimation={hDecim}
            suppressImage={suppressImage}
            onSuppressImage={hSuppressImage}
            weighting={weighting}
            onWeighting={hWeighting}
            overlap={overlap}
            onOverlap={hOverlap}
            taperCount={taperCount}
            onTaperCount={hTaperCount}
            historyCapacity={historyCapacity}
            onHistoryCapacity={hHistoryCapacity}
            historyChunk={historyChunk}
            onHistoryChunk={hHistoryChunk}
            snapStats={snapStats}
            centerFreq={backendCenterFreq}
            spectrumTwoSided={backendSpectrumTwoSided}
            peakMinLevel={peakMinLevel}
            onPeakMinLevel={hPeakMinLevel}
            onExport={onExport}
          />
        )}
      </div>
      <StatusBar
        cursorF1={cursorF1}
        sampleRate={sampleRate}
        fftSize={fftSize}
        backendConnected={backendConnected}
        srcLabel={srcLabel}
        backendFrameId={backendFrameId}
        timeRef={timeRef}
        fileLooping={backendSourceMode === 2 && backendLoopCount > 0}
        loopCount={backendLoopCount}
      />
      {fileModal && (
        <FileLoadModal
          filePath={fileModal.path}
          format={fileModal.format}
          hint={fileModal.hint}
          onSubmit={onFileModalSubmit}
          onCancel={() => setFileModal(null)}
        />
      )}
    </div>
  );
}
