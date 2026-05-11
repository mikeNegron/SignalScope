import { T, PEAK_PALETTE } from "../../lib/tokens.js";
import { Section, NumInput } from "../controls.jsx";
import { fmtFreqLong } from "../../lib/freq.js";

// Peaks tab - table of detected peaks (color-matched to the circles drawn
// on the spectrum) plus a single knob that tunes how much a candidate has
// to rise above its local floor before the backend reports it. Peaks come
// from the SpectrumStats frame, snapshotted at 4 Hz.
// Props:
//   peaks   - Array<{ binIdx, value, prom, N }>, strongest -> weakest
//   noiseFloor, fundamentalFreq - global stats from the same snapshot
//   sampleRate, centerFreq, spectrumTwoSided - for bin -> Hz mapping
//   peakMinProm, onPeakMinProm - slider state and handler
export function PeaksTab({
  peaks = [],
  noiseFloor,
  fundamentalFreq,
  sampleRate,
  centerFreq = 0,
  spectrumTwoSided = false,
  peakMinLevel = -60,
  onPeakMinLevel,
}) {
  const binToFreq = (bin, N) => {
    const t = (bin + 0.5) / N;
    return spectrumTwoSided
      ? centerFreq + (t - 0.5) * sampleRate
      : t * (sampleRate / 2);
  };

  return (
    <div style={{ display: "flex", flexDirection: "column", gap: 10 }}>
      <Section label="Detection">
        <NumInput
          label="Min Level"
          value={peakMinLevel}
          onChange={onPeakMinLevel}
          min={-130}
          max={0}
          step={0.5}
          suffix="dBFS"
          width={70}
        />
        <div style={{ fontSize: 9, color: T.textMuted, marginTop: 4 }}>
          Same scale as the spectrum y-axis. Only bins above this level
          qualify. e.g. −26.5 reports anything stronger than that.
        </div>
      </Section>

      <Section label={`Detected (${peaks.length})`}>
        {peaks.length === 0 ? (
          <div style={{ fontSize: 10, color: T.textMuted, padding: "4px 0" }}>
            No peaks above the prominence gate.
          </div>
        ) : (
          <div style={{ display: "flex", flexDirection: "column", gap: 2 }}>
            <PeakHeader />
            {peaks.map((p, i) => (
              <PeakRow
                key={i}
                index={i}
                color={PEAK_PALETTE[i % PEAK_PALETTE.length]}
                freq={binToFreq(p.binIdx, p.N)}
                value={p.value}
                prom={p.prom}
              />
            ))}
          </div>
        )}
      </Section>

      {(isFinite(noiseFloor) || (fundamentalFreq && fundamentalFreq > 0)) && (
        <Section label="Stats">
          {isFinite(noiseFloor) && (
            <Row label="Noise floor" value={`${noiseFloor.toFixed(1)} dB`} />
          )}
          {fundamentalFreq > 0 && (
            <Row
              label="Fundamental"
              value={fmtFreqLong(fundamentalFreq, 2)}
              color={T.orange}
            />
          )}
        </Section>
      )}
    </div>
  );
}

// Internals
function PeakHeader() {
  return (
    <div
      style={{
        display: "grid",
        gridTemplateColumns: "10px 1fr 56px 50px",
        gap: 6,
        fontSize: 9,
        color: T.textMuted,
        textTransform: "uppercase",
        padding: "0 2px 2px",
        borderBottom: `1px solid ${T.borderSub}`,
      }}
    >
      <span />
      <span>Frequency</span>
      <span style={{ textAlign: "right" }}>Level</span>
      <span style={{ textAlign: "right" }}>Prom</span>
    </div>
  );
}

function PeakRow({ index, color, freq, value, prom }) {
  return (
    <div
      style={{
        display: "grid",
        gridTemplateColumns: "10px 1fr 56px 50px",
        gap: 6,
        fontSize: 10,
        fontFamily: T.font,
        color: T.text,
        padding: "3px 2px",
        borderRadius: 2,
        background: index === 0 ? T.bgElev : "transparent",
        alignItems: "center",
      }}
      title={`Peak #${index + 1}`}
    >
      <span
        style={{
          width: 8,
          height: 8,
          borderRadius: "50%",
          background: color,
          boxShadow: `0 0 4px ${color}80`,
        }}
      />
      <span>{fmtFreqLong(freq, 2)}</span>
      <span style={{ textAlign: "right", color: T.textMid }}>
        {value.toFixed(1)} dB
      </span>
      <span style={{ textAlign: "right", color: T.textMuted }}>
        {prom.toFixed(1)}
      </span>
    </div>
  );
}

function Row({ label, value, color }) {
  return (
    <div
      style={{
        display: "flex",
        justifyContent: "space-between",
        alignItems: "baseline",
        padding: "2px 0",
        borderBottom: `1px solid ${T.borderSub}`,
        fontSize: 10,
        fontFamily: T.font,
      }}
    >
      <span style={{ color: T.textMid }}>{label}</span>
      <span style={{ color: color || T.text }}>{value}</span>
    </div>
  );
}
