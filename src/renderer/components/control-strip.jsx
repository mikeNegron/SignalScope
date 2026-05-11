import { T, CMAPS } from "../lib/tokens.js";
import { Sel, Knob, Btn } from "./controls.jsx";

const PANEL_KEYS = ["spectrogram", "spectrum", "waveform", "iq"];

export function ControlStrip({
  fftSize,
  windowFn,
  colorMap,
  gain,
  avgCount,
  showPeaks,
  periodicityBars,
  peakHold,
  cursorF1,
  cursorF2,
  panels,
  onFFT,
  onWin,
  onColorMap,
  onGain,
  onAvg,
  onShowPeaks,
  onPeriodicityBars,
  onPeakHold,
  onClearPeakHold,
  onClearF1,
  onClearF2,
  onClearCursors,
  onTogglePanel,
}) {
  return (
    <div
      style={{
        display: "flex",
        alignItems: "center",
        gap: 10,
        padding: "3px 12px",
        borderBottom: `1px solid ${T.borderSub}`,
        background: T.bgPanel,
        flexShrink: 0,
        flexWrap: "wrap",
      }}
    >
      <Sel
        label="FFT"
        value={fftSize}
        onChange={onFFT}
        options={["256", "512", "1024", "2048", "4096", "8192", "16384", "32768", "65536"]}
      />
      <Sel
        label="WIN"
        value={windowFn}
        onChange={onWin}
        options={["hanning", "hamming", "blackman", "flattop", "kaiser", "rectangular"]}
      />
      <Sel label="MAP" value={colorMap} onChange={onColorMap} options={Object.keys(CMAPS)} />
      <Knob label="GAIN" value={gain} onChange={onGain} min={-40} max={40} unit="dB" numeric />
      <Knob label="AVG" value={avgCount} onChange={onAvg} min={1} max={64} />
      <div style={{ width: 1, height: 14, background: T.border }} />
      <Btn active={showPeaks} onClick={onShowPeaks}>PEAKS</Btn>
      <Btn active={periodicityBars} onClick={onPeriodicityBars} color={T.orange}>
        HARMONICS
      </Btn>
      <Btn active={peakHold} onClick={onPeakHold} color={T.info}>HOLD</Btn>
      {peakHold && (
        <Btn onClick={onClearPeakHold} style={{ padding: "1px 6px", fontSize: 9 }}>
          CLR HOLD
        </Btn>
      )}
      <div style={{ width: 1, height: 14, background: T.border }} />
      <Btn active={cursorF1 != null} onClick={onClearF1} color={T.pink}>f1</Btn>
      <Btn active={cursorF2 != null} onClick={onClearF2} color={T.purple}>f2</Btn>
      <Btn onClick={onClearCursors}>CLR</Btn>
      <div style={{ flex: 1 }} />
      {PANEL_KEYS.concat(["sidebar"]).map((k) => (
        <Btn
          key={k}
          active={panels[k]}
          onClick={() => onTogglePanel(k)}
          style={{ padding: "1px 5px", fontSize: 9 }}
        >
          {k === "iq" ? "IQ" : k[0].toUpperCase() + k.slice(1)}
        </Btn>
      ))}
    </div>
  );
}
