import React, { useState, useEffect, useRef, useCallback } from "react";
import { createRoot } from "react-dom/client";
import SignalAnalyzer from "./signal-analyzer.jsx";
import { parseFrame } from "./lib/wire-parse.js";
import "./styles/global.css";

// Data arrives at potentially hundreds of fps; buffer into a ref and
// flush to React once per RAF. Zero setState inside onmessage.
function useBackendConnection(url = "ws://localhost:8765") {
  const wsRef = useRef(null);
  const gotDataRef = useRef(false);
  const pausedRef = useRef(false);
  const reconnectTimer = useRef(null); // FIX: track reconnect timeout

  // Buffer - written by onmessage (any speed), read directly by GPU RAF loop.
  // Arrays are pre-allocated and reused with .set() so onmessage never allocates
  // on the steady-state path. _iqRe/_iqIm are the backing arrays; iq.re/iq.im
  // point into them so callers see a stable { re, im } object.
  const buf = useRef({
    spectrum: null, waveform: null, iq: null, phase: null, histogram: null,
    spectrumHold: null, // peak-hold trace, only populated while backend hold is on
    _iqRe: null, _iqIm: null,
    frameId: 0, clipping: false, sampleRate: 48000,
    specSeq: 0, wavSeq: 0, holdSeq: 0, statsSeq: 0, // monotonic counters per stream
    // Spectrum/phase span when the source is complex/IQ - see FrameFlags.
    spectrumTwoSided: false,
    sourceIsComplex: false,
    // Source-status frame (file source loop counter, current source mode).
    loopCount: 0,
    sourceMode: 0, // 0=test, 1=mic, 2=file
    centerFreq: 0, // Hz; non-zero for SDR/SigMF captures
    // Spectrum analytics (computed in C++; replaces the old JS peak detector
    // and useMeasurements hook).
    peaks: [],            // Array<{ binIdx, value, prom, N }>
    noiseFloor: -120,     // dBFS, median of finite spectrum bins
    fundamentalFreq: 0,   // Hz; 0 when no peaks
  });

  // Only UI-relevant state goes through React (status bar, sidebar).
  const [connected, setConnected] = useState(false);
  const [frameId, setFrameId] = useState(0);
  const [clipping, setClipping] = useState(false);
  const [backendSampleRate, setBackendSampleRate] = useState(48000);
  const [loopCount, setLoopCount] = useState(0);
  const [sourceMode, setSourceMode] = useState(0);
  const [sourceComplex, setSourceComplex] = useState(false);
  // Separate from sourceComplex: the spectrum frame can be two-sided even
  // for a real source (DDC with tune≠0, Hilbert pre-stage). The renderer
  // uses this for axis math and to gate log-scale availability; sourceComplex
  // gates the Hilbert toggle.
  const [spectrumTwoSided, setSpectrumTwoSided] = useState(false);
  const [centerFreq, setCenterFreq] = useState(0);

  const setPaused = useCallback((p) => {
    pausedRef.current = p;
  }, []);

  // Status state (frameId/clipping/sampleRate) is consumed only by the status bar
  // and sidebar - flushing at 4Hz is plenty. Doing it on rAF was forcing a full
  // React re-render of App -> SignalAnalyzer -> children at 60fps, which by itself
  // generates significant GC pressure (new JSX closures, prop objects, etc.).
  useEffect(() => {
    const id = setInterval(() => {
      if (pausedRef.current) return;
      const b = buf.current;
      setFrameId(b.frameId);
      setClipping(b.clipping);
      setBackendSampleRate(b.sampleRate);
      setLoopCount(b.loopCount);
      setSourceMode(b.sourceMode);
      setSourceComplex(b.sourceIsComplex);
      setSpectrumTwoSided(b.spectrumTwoSided);
      setCenterFreq(b.centerFreq);
    }, 250);
    return () => clearInterval(id);
  }, []);

  const connect = useCallback(() => {
    const prev = wsRef.current;
    if (prev && (prev.readyState === WebSocket.OPEN || prev.readyState === WebSocket.CONNECTING)) return;
    if (prev) {
      prev.onclose = null; // prevent the old onclose from firing another reconnect
      prev.close();
    }

    const ws = new WebSocket(url);
    ws.binaryType = "arraybuffer";

    ws.onopen = () => {
      console.log("[ws] Socket opened, waiting for data...");
    };

    ws.onmessage = (event) => {
      if (!(event.data instanceof ArrayBuffer) || event.data.byteLength < 12)
        return;

      if (!gotDataRef.current) {
        gotDataRef.current = true;
        setConnected(true);
        console.log("[ws] Backend confirmed - receiving data");
      }

      // Frame decoding lives in lib/wire-parse.js so it's testable in
      // isolation. The WebSocket lifecycle (open/close/reconnect) stays
      // here.
      parseFrame(buf.current, event.data);
    };

    ws.onclose = (event) => {
      console.log(`[ws] Disconnected (code: ${event.code})`);
      gotDataRef.current = false;
      setConnected(false);
      // Null out data fields so the GPU RAF loop stops drawing immediately.
      // _iqRe/_iqIm are nulled too: keeping them alive saved one allocation
      // on reconnect, but a rapid reconnect could briefly render OLD IQ
      // alongside the NEW spectrum/waveform. wire-parse.js reallocates on
      // the first post-reconnect IQ frame (length-check in its IQ branch).
      const b = buf.current;
      b.spectrum = null; b.waveform = null; b.iq = null;
      b._iqRe = null; b._iqIm = null;
      b.phase = null; b.histogram = null;
      b.spectrumHold = null;
      b.frameId = 0; b.clipping = false; b.sampleRate = 48000;
      b.specSeq = 0; b.wavSeq = 0; b.holdSeq = 0; b.statsSeq = 0;
      b.spectrumTwoSided = false; b.sourceIsComplex = false;
      setSpectrumTwoSided(false);
      b.loopCount = 0; b.sourceMode = 0; b.centerFreq = 0;
      b.peaks.length = 0; b.noiseFloor = -120; b.fundamentalFreq = 0;
      // FIX: track the timeout so cleanup can cancel it
      reconnectTimer.current = setTimeout(() => connect(), 300);
    };

    ws.onerror = () => {};
    wsRef.current = ws;
  }, [url]);

  // `value` may be a primitive (sent as { cmd, value }) or an object whose
  // keys are spread flat into the message (used by load_file which needs
  // path/format/datatype/sample_rate/channels alongside the cmd).
  const sendCommand = useCallback((cmd, value) => {
    if (wsRef.current?.readyState !== WebSocket.OPEN) return;
    const msg = value && typeof value === "object" && !Array.isArray(value)
      ? JSON.stringify({ cmd, ...value })
      : JSON.stringify({ cmd, value });
    wsRef.current.send(msg);
  }, []);

  useEffect(() => {
    connect();
    return () => {
      // FIX: cancel pending reconnect timeout to prevent an infinite
      // reconnect loop after unmount (each onclose would schedule another
      // connect() into the void, leaking WebSocket + buffer allocations).
      clearTimeout(reconnectTimer.current);
      if (wsRef.current) {
        wsRef.current.onclose = null; // prevent onclose from scheduling another reconnect
        wsRef.current.close();
        wsRef.current = null;
      }
    };
  }, [connect]);

  return {
    connected,
    bufRef: buf,  // raw ref - GPU RAF reads display data directly, bypassing React
    frameId,
    clipping,
    backendSampleRate,
    loopCount,
    sourceMode,
    sourceComplex,
    spectrumTwoSided,
    centerFreq,
    sendCommand,
    reconnect: connect,
    setPaused,
  };
}

function App() {
  const [wsUrl, setWsUrl] = useState("ws://localhost:8765");
  // False once we know a backend process is running - suppresses simulation.
  // Defaults true so simulation shows when there is genuinely no backend.
  const [simulationEnabled, setSimulationEnabled] = useState(true);

  // Kept in sync with backend.reconnect so the onBackendConfig handler can
  // trigger an immediate connection attempt without being inside the effect.
  const connectRef = useRef(null);

  const backend = useBackendConnection(wsUrl);
  connectRef.current = backend.reconnect;

  useEffect(() => {
    if (window.signalscope) {
      window.signalscope.getBackendConfig().then((cfg) => {
        if (cfg?.wsUrl) setWsUrl(cfg.wsUrl);
        if (cfg?.running) setSimulationEnabled(false);
      });
      window.signalscope.onBackendConfig((cfg) => {
        if (cfg?.wsUrl) setWsUrl(cfg.wsUrl);
        if (cfg?.running) {
          setSimulationEnabled(false);
          // Backend just confirmed it's listening - connect now rather than
          // waiting for the next retry cycle.
          connectRef.current?.();
        }
      });
    }
  }, []);
  window.__debug_connected = backend.connected;

  // FIX: stable callback reference - avoids recreating SignalAnalyzer's hMode
  // on every render and won't defeat React.memo if it's ever applied.
  const handlePause = useCallback((paused) => {
    backend.setPaused(paused);
    backend.sendCommand("set_pause", paused);
  }, [backend.setPaused, backend.sendCommand]);

  return (
    <SignalAnalyzer
      simulationEnabled={simulationEnabled}
      backendConnected={backend.connected}
      wsDataRef={backend.bufRef}
      backendFrameId={backend.frameId}
      backendClipping={backend.clipping}
      backendSampleRate={backend.backendSampleRate}
      backendLoopCount={backend.loopCount}
      backendSourceMode={backend.sourceMode}
      backendSourceComplex={backend.sourceComplex}
      backendSpectrumTwoSided={backend.spectrumTwoSided}
      backendCenterFreq={backend.centerFreq}
      onBackendCommand={backend.sendCommand}
      onReconnect={backend.reconnect}
      onPause={handlePause}
    />
  );
}

createRoot(document.getElementById("root")).render(<App />);