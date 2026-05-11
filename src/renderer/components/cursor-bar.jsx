import { T, safeSpecRead } from "../lib/tokens.js";
import { fmtFreqLong } from "../lib/freq.js";

export function CursorInfoBar({
  cursorF1, cursorF2, sampleRate, fftSize, centerFreq = 0,
  // Latest spectrum snapshot - used to read off the dB value at each
  // cursor's frequency. Optional; when absent, the dB readout is hidden.
  spectrumData = null,
  twoSided = false,
}) {
  const ok1 = cursorF1 != null && isFinite(cursorF1);
  const ok2 = cursorF2 != null && isFinite(cursorF2);
  const both = ok1 && ok2;
  const delta = both ? Math.abs(cursorF2 - cursorF1) : 0;
  const center = both ? (cursorF1 + cursorF2) / 2 : 0;
  const binRes = sampleRate / +fftSize;
  const s = { fontFamily: T.font, fontSize: 9 };

  // dB at each cursor's frequency, sampled from the latest spectrum
  // snapshot. snapSpectrum updates at ~4 Hz so this matches the rest of
  // the sidebar's refresh cadence - no extra subscription needed.
  const db1 = ok1 && spectrumData
    ? safeSpecRead(spectrumData, cursorF1, sampleRate, centerFreq, twoSided)
    : null;
  const db2 = ok2 && spectrumData
    ? safeSpecRead(spectrumData, cursorF2, sampleRate, centerFreq, twoSided)
    : null;

  return (
    <div
      style={{
        display: "flex",
        gap: 14,
        alignItems: "center",
        padding: "0 12px",
        height: 22,
        borderTop: `1px solid ${T.borderSub}`,
        background: T.bgElev,
        flexShrink: 0,
        overflow: "hidden",
      }}
    >
      <span style={{ ...s, color: T.pink }}>
        f1: {ok1 ? fmtFreqLong(cursorF1) : "-"}
        {db1 != null && ` · ${db1.toFixed(1)} dB`}
      </span>
      <span style={{ ...s, color: T.purple }}>
        f2: {ok2 ? fmtFreqLong(cursorF2) : "-"}
        {db2 != null && ` · ${db2.toFixed(1)} dB`}
      </span>
      {both && (
        <>
          <span style={{ width: 1, height: 12, background: T.border }} />
          <span style={{ ...s, color: T.primary }}>
            Δf: {fmtFreqLong(delta)}
          </span>
          <span style={{ ...s, color: T.success }}>
            Center: {fmtFreqLong(center)}
          </span>
          <span style={{ ...s, color: T.info }}>
            Ratio:{" "}
            {Math.min(cursorF1, cursorF2) > 0
              ? (
                  Math.max(cursorF1, cursorF2) / Math.min(cursorF1, cursorF2)
                ).toFixed(4)
              : "∞"}
          </span>
          {binRes > 0 && (
            <span style={{ ...s, color: T.orange }}>
              ΔBins: {(delta / binRes).toFixed(1)}
            </span>
          )}
        </>
      )}
      {centerFreq > 0 && (
        <span style={{ ...s, color: T.textMuted, marginLeft: "auto" }}>
          Fc: {fmtFreqLong(centerFreq)}
        </span>
      )}
    </div>
  );
}
