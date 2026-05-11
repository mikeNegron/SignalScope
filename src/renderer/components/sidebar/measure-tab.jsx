import { T } from "../../lib/tokens.js";
import { Section, MRow } from "../controls.jsx";

export function MeasureTab({
  meas,
  backendClipping,
  backendConnected,
  backendFrameId,
  timeRef,
  srcLabel,
}) {
  if (!meas) return null;
  return (
    <div style={{ display: "flex", flexDirection: "column", gap: 10 }}>
      {backendClipping && (
        <div
          style={{
            background: T.error + "22",
            border: `1px solid ${T.error}44`,
            borderRadius: 4,
            padding: "4px 8px",
            fontFamily: T.font,
            fontSize: 10,
            color: T.error,
            textAlign: "center",
          }}
        >
          ⚠ CLIPPING
        </div>
      )}
      <Section label="Peaks">
        <MRow label="Primary" value={meas.pkF.toFixed(1)} unit="Hz" color={T.primary} />
        <MRow label="Level" value={meas.pkV.toFixed(1)} unit="dBFS" color={T.warning} />
        <MRow label="Secondary" value={meas.pk2F.toFixed(1)} unit="Hz" />
        <MRow label="Δ Freq" value={meas.delta.toFixed(1)} unit="Hz" color={T.info} />
      </Section>
      <Section label="Fundamental">
        <MRow label="Freq" value={meas.pkF.toFixed(3)} unit="Hz" color={T.success} />
        <MRow
          label="Period"
          value={meas.pkF > 0 ? (1000 / meas.pkF).toFixed(4) : "∞"}
          unit="ms"
        />
      </Section>
      <Section label="Distortion">
        <MRow label="SNR" value={meas.snr.toFixed(1)} unit="dB" color={T.info} />
        <MRow label="THD" value="-62.3" unit="dB" />
        <MRow label="SINAD" value="58.7" unit="dB" />
        <MRow label="ENOB" value="9.4" unit="bits" />
      </Section>
      <Section label="System">
        <MRow
          label="Source"
          value={srcLabel}
          unit=""
          color={backendConnected ? T.success : T.orange}
        />
        <MRow
          label="Frame"
          value={backendConnected ? backendFrameId : Math.floor(timeRef.current * 20)}
          unit=""
        />
      </Section>
    </div>
  );
}
