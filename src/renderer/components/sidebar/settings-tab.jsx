import { T } from "../../lib/tokens.js";
import { Knob, Sel, Btn, Section, NumInput } from "../controls.jsx";
import { exportPNG } from "../../lib/export.js";
import { useState, useCallback, useEffect } from "react";
import { Toast } from "../Toast.jsx";

// Reconnect-await deadline for the Diagnostics Restart/Start flows.
// The IPC resolves when the main-process spawn returns; the renderer
// then waits up to this long for the WS to handshake before erroring.
const RECONNECT_TIMEOUT_MS = 10000;

export function SettingsTab({
  brightness,
  contrast,
  scrollSpeed,
  onBrightness,
  onContrast,
  onScrollSpeed,
  waveformData,
  spectrumData,
  sampleRate,
  // Backend-driven export. signal-analyzer wires this up: shows a save
  // dialog, then sends an export_capture command. We keep the data
  // refs above so the buttons can disable when there's nothing to save.
  onExport,
  freqScale = "linear",
  onFreqScale,
  // Source is two-sided/IQ - log axis is meaningless (negative frequencies)
  // so the control falls back to linear and shows a hint.
  freqScaleDisabled = false,
  // Down-mixer state (DDC). Tune is in Hz; decimation is an integer ≥1.
  tuneHz = 0,
  decimation = 1,
  onTune,
  onDecimation,
  // Hilbert pre-stage toggle: makes a real-input DDC produce one peak per
  // tone instead of mirror pairs at ±f. No-op when source is already
  // complex; the toggle is shown disabled in that case.
  suppressImage = false,
  onSuppressImage,
  sourceComplex = false,
  // Equalization curve. Renderer never applies this - the backend mutates
  // the spectrum dB values so peaks/noise-floor stay consistent with what
  // the user sees. Currently supported: "none" | "flatten" | "A-weight" | "C-weight".
  weighting = "none",
  onWeighting,
  // FFT window-stride overlap, in percent (0/50/75). 0 = back-to-back
  // windows; higher = each FFT shares more samples with the previous,
  // giving finer time resolution on the spectrogram at the cost of more
  // computation per second.
  overlap = 0,
  onOverlap,
  // Multitaper count K (1, 2, 4, 8). 1 = single configured window.
  // K>1 averages K orthogonal sine-taper FFTs per frame - variance
  // drops by ~1/K (cleaner noise floor) at K× the FFT cost.
  taperCount = 1,
  onTaperCount,
  // Deep-history settings - backend ring size + per-request chunk.
  historyCapacity = 4096,
  onHistoryCapacity,
  historyChunk = 512,
  onHistoryChunk,
  backendConnected = false,
}) {
  // Diagnostics: backend lifecycle controls. The IPC handlers + preload
  // bridge are wired in main.js / preload.js; we surface them here.
  //
  // toast = null | one of:
  //   { kind: 'pending', msg }                                  — IPC in flight
  //   { kind: 'pending', msg, awaiting: { successMsg, deadline } } — waiting for WS reconnect
  //   { kind: 'success', msg }
  //   { kind: 'error',   msg }
  //
  // The split-pending shape exists because the IPC resolves when the
  // main-process spawn returns; the WS reconnect lands a few hundred ms
  // later. Without the awaiting phase, "success" would flash before the
  // backend is actually serving frames.
  const [toast, setToast] = useState(null);
  const dismissToast = useCallback(() => setToast(null), []);

  // When we're awaiting a reconnect and the WS lands, promote pending → success.
  useEffect(() => {
    if (toast?.kind !== "pending" || !toast.awaiting) return;
    if (backendConnected) {
      setToast({ kind: "success", msg: toast.awaiting.successMsg });
    }
  }, [backendConnected, toast]);

  // Watchdog: error out if the WS doesn't come back within the deadline.
  useEffect(() => {
    if (toast?.kind !== "pending" || !toast.awaiting) return;
    const remaining = toast.awaiting.deadline - Date.now();
    if (remaining <= 0) {
      setToast({ kind: "error", msg: "Backend did not reconnect in time" });
      return;
    }
    const t = setTimeout(() => {
      setToast({ kind: "error", msg: "Backend did not reconnect in time" });
    }, remaining);
    return () => clearTimeout(t);
  }, [toast]);

  const runLifecycle = useCallback(async ({ action, pendingMsg, successMsg, awaitReconnect }) => {
    // Re-entrancy guard: ignore clicks while an action is in flight.
    if (toast?.kind === "pending") return;

    const bridge = window.signalscope?.[action];
    if (typeof bridge !== "function") {
      setToast({ kind: "error", msg: `Bridge function ${action} unavailable` });
      return;
    }

    setToast({ kind: "pending", msg: pendingMsg });

    let result;
    try {
      result = await bridge();
    } catch (err) {
      setToast({ kind: "error", msg: err?.message || String(err) });
      return;
    }

    // The IPC handlers return { success: true|false, reason?, already? }.
    if (result?.success === false) {
      setToast({ kind: "error", msg: result.reason || `${action} failed` });
      return;
    }

    // Start short-circuits with { already: true } if backend is already running.
    if (awaitReconnect && !result?.already) {
      setToast({
        kind: "pending",
        msg: "Waiting for backend to reconnect...",
        awaiting: {
          successMsg,
          deadline: Date.now() + RECONNECT_TIMEOUT_MS,
        },
      });
      return;
    }

    setToast({ kind: "success", msg: successMsg });
  }, [toast]);

  const onRestart = useCallback(
    () => runLifecycle({
      action: "restartBackend",
      pendingMsg: "Restarting backend...",
      successMsg: "Backend restarted",
      awaitReconnect: true,
    }),
    [runLifecycle],
  );
  const onStart = useCallback(
    () => runLifecycle({
      action: "startBackend",
      pendingMsg: "Starting backend...",
      successMsg: "Backend started",
      awaitReconnect: true,
    }),
    [runLifecycle],
  );
  const onStop = useCallback(
    () => runLifecycle({
      action: "stopBackend",
      pendingMsg: "Stopping backend...",
      successMsg: "Backend stopped",
      awaitReconnect: false,
    }),
    [runLifecycle],
  );

  // Clamp the Tune slider to ±fs/2 for the current source rate so the user
  // can't drag past Nyquist.
  const tuneMax = Math.max(1, Math.round(sampleRate / 2));
  const ddcActive = tuneHz !== 0 || decimation > 1;
  const effectiveSr = sampleRate / Math.max(1, decimation);
  return (
    <div style={{ display: "flex", flexDirection: "column", gap: 12 }}>
      <Section label="Display">
        <div style={{ display: "flex", flexDirection: "column", gap: 6 }}>
          <Knob
            label="Brightness"
            value={brightness}
            onChange={onBrightness}
            min={0}
            max={100}
            unit="%"
          />
          <Knob
            label="Contrast"
            value={contrast}
            onChange={onContrast}
            min={10}
            max={200}
            unit="%"
          />
          <Knob
            label="Scroll Spd"
            value={scrollSpeed}
            onChange={onScrollSpeed}
            min={1}
            max={10}
          />
        </div>
      </Section>
      <Section label="Freq Scale">
        <Sel
          label=""
          value={freqScaleDisabled ? "linear" : freqScale}
          onChange={onFreqScale}
          options={[
            { value: "linear", label: "linear" },
            { value: "log",    label: "logarithmic" },
          ]}
        />
        {freqScaleDisabled && (
          <div style={{ fontSize: 9, color: T.textMuted, marginTop: 4 }}>
            Two-sided IQ source - log scale unavailable.
          </div>
        )}
      </Section>
      <Section label="Down Mixer">
        <div style={{ display: "flex", flexDirection: "column", gap: 6 }}>
          <NumInput
            label="Tune"
            value={tuneHz}
            onChange={onTune}
            min={-tuneMax}
            max={tuneMax}
            step={1}
            suffix="Hz"
            width={90}
          />
          <NumInput
            label="Decimate"
            value={decimation}
            onChange={onDecimation}
            min={1}
            max={64}
            step={1}
            width={50}
          />
          <Btn
            active={suppressImage && !sourceComplex}
            onClick={sourceComplex ? undefined : onSuppressImage}
            title={sourceComplex
              ? "Source is already complex - Hilbert pre-stage not needed"
              : "Insert a Hilbert transformer in front of the DDC so each tone produces one peak instead of mirrored pairs at ±f."}
            style={sourceComplex ? { opacity: 0.4, cursor: "not-allowed" } : undefined}
          >
            Suppress Image
          </Btn>
        </div>
        {ddcActive && (
          <div style={{ fontSize: 9, color: T.textMuted, marginTop: 4 }}>
            Effective fs: {effectiveSr.toFixed(0)} Hz
            {decimation > 1 && ` (M=${decimation})`}
          </div>
        )}
      </Section>
      <Section label="FFT Overlap">
        <Sel
          label=""
          value={String(overlap)}
          onChange={(v) => onOverlap?.(parseInt(v, 10))}
          options={[
            { value: "0",  label: "0% (no overlap)" },
            { value: "50", label: "50%" },
            { value: "75", label: "75%" },
          ]}
        />
        {overlap > 0 && (
          <div style={{ fontSize: 9, color: T.textMuted, marginTop: 4 }}>
            Each FFT advances by {100 - overlap}% of fft_size - finer
            spectrogram time axis.
          </div>
        )}
      </Section>
      <Section label="History">
        <div style={{ display: "flex", flexDirection: "column", gap: 6 }}>
          <NumInput
            label="Depth"
            value={historyCapacity}
            onChange={onHistoryCapacity}
            min={256}
            max={65536}
            step={256}
            suffix="rows"
            width={90}
          />
          <NumInput
            label="Chunk"
            value={historyChunk}
            onChange={onHistoryChunk}
            min={64}
            max={8192}
            step={64}
            suffix="rows"
            width={90}
          />
          <div style={{ fontSize: 9, color: T.textMuted, marginTop: 2 }}>
            Pause + scroll wheel on the spectrogram to scrub back in time.
            Alt+wheel = freq zoom, Shift+wheel = time zoom. Larger chunk
            = fewer round-trips while panning.
          </div>
        </div>
      </Section>
      <Section label="Multitaper">
        <Sel
          label=""
          value={String(taperCount)}
          onChange={(v) => onTaperCount?.(parseInt(v, 10))}
          options={[
            { value: "1", label: "1 (single window)" },
            { value: "2", label: "2 tapers" },
            { value: "4", label: "4 tapers" },
            { value: "8", label: "8 tapers" },
          ]}
        />
        {taperCount > 1 && (
          <div style={{ fontSize: 9, color: T.textMuted, marginTop: 4 }}>
            Averages {taperCount} sine-taper FFTs per frame - variance
            ~÷{taperCount}, main lobe slightly wider, {taperCount}× FFT cost.
          </div>
        )}
      </Section>
      <Section label="Equalization">
        <Sel
          label=""
          value={weighting}
          onChange={onWeighting}
          options={[
            { value: "none",     label: "none" },
            { value: "flatten",  label: "flatten" },
            { value: "A-weight", label: "A-weight" },
            { value: "C-weight", label: "C-weight" },
          ]}
        />
        {weighting !== "none" && (
          <div style={{ fontSize: 9, color: T.textMuted, marginTop: 4 }}>
            {weighting === "flatten"
              ? "Auto-range the y-axis to the live spectrum (data unchanged)."
              : weighting === "A-weight"
              ? "IEC 61672 A-weight (psychoacoustic, low SPL)."
              : "IEC 61672 C-weight (low/high cut, high SPL)."}
          </div>
        )}
      </Section>
      <Section label="Export">
        <div style={{ display: "flex", gap: 4, flexWrap: "wrap" }}>
          <Btn
            onClick={() => onExport?.("wav", "waveform")}
            active={!!waveformData && !!onExport}
            title="Waveform -> WAV float32 (backend write)"
            testid="export-wav"
          >
            WAV
          </Btn>
          <Btn
            onClick={() => onExport?.("csv", "spectrum")}
            active={!!spectrumData && !!onExport}
            title="Spectrum -> CSV (backend write)"
            testid="export-csv"
          >
            CSV
          </Btn>
          <Btn onClick={exportPNG} active title="Screenshot largest canvas" testid="export-png">
            PNG
          </Btn>
          <Btn
            onClick={() => onExport?.("aiff", "waveform")}
            active={!!waveformData && !!onExport}
            title="Waveform -> AIFF int16 (backend write)"
            testid="export-aiff"
          >
            AIFF
          </Btn>
          <Btn
            onClick={() => onExport?.("raw", "spectrum")}
            active={!!spectrumData && !!onExport}
            title="Spectrum -> raw float32 (backend write)"
            testid="export-raw"
          >
            RAW
          </Btn>
        </div>
        <div
          style={{
            fontSize: 9,
            fontFamily: T.font,
            color: T.textMuted,
            marginTop: 6,
          }}
        >
          WAV/AIFF: waveform · CSV/RAW: spectrum · PNG: capture
        </div>
      </Section>
      <Section label="Diagnostics">
        <div style={{ display: "flex", gap: 6, flexWrap: "wrap" }}>
          <Btn
            onClick={onRestart}
            active
            title="Stop and immediately restart the backend process"
            testid="diagnostics-restart"
          >
            Restart Backend
          </Btn>
          <Btn
            onClick={onStart}
            active={!backendConnected}
            title={backendConnected
              ? "Backend is already running"
              : "Start the backend process"}
            testid="diagnostics-start"
          >
            Start
          </Btn>
          <Btn
            onClick={onStop}
            active={backendConnected}
            title={backendConnected
              ? "Stop the backend process"
              : "Backend is not running"}
            testid="diagnostics-stop"
          >
            Stop
          </Btn>
        </div>
      </Section>
      {toast && (
        <Toast kind={toast.kind} msg={toast.msg} onDismiss={dismissToast} />
      )}
    </div>
  );
}
