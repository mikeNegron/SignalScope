import { useMemo } from "react";
import { T, safeSpecRead } from "../../lib/tokens.js";
import { Btn, CursorCard, Section, MRow } from "../controls.jsx";

export function CursorsTab({
  cursorF1,
  cursorF2,
  spectrumData,
  sampleRate,
  fftSize,
  onClearF1,
  onClearF2,
  onClearAll,
}) {
  const deltaBlock = useMemo(() => {
    const ok1 = cursorF1 != null && isFinite(cursorF1);
    const ok2 = cursorF2 != null && isFinite(cursorF2);
    if (!ok1 || !ok2) return null;
    const df = Math.abs(cursorF2 - cursorF1);
    const cf = (cursorF1 + cursorF2) / 2;
    const binRes = sampleRate / +fftSize;
    const ratio = cursorF1 > 0 ? (cursorF2 / cursorF1).toFixed(6) : "∞";
    const dBins = binRes > 0 ? (df / binRes).toFixed(2) : "0";
    const v1 = safeSpecRead(spectrumData, cursorF1, sampleRate);
    const v2 = safeSpecRead(spectrumData, cursorF2, sampleRate);
    return (
      <div
        style={{
          background: T.bgElev,
          border: `1px solid ${T.primary}33`,
          borderRadius: 4,
          padding: "8px 10px",
        }}
      >
        <Section label="Delta Analysis" color={T.primary}>
          <MRow label="Δ Frequency" value={df.toFixed(3)} unit="Hz" color={T.primary} />
          <MRow label="Center Freq" value={cf.toFixed(3)} unit="Hz" color={T.success} />
          <MRow label="Ratio f2/f1" value={ratio} unit="" color={T.orange} />
          <MRow label="Δ Bins" value={dBins} unit="" />
          <MRow label="BW est" value={df.toFixed(1)} unit="Hz" color={T.info} />
          <MRow label="Δ Level" value={(v1 - v2).toFixed(1)} unit="dB" color={T.warning} />
        </Section>
      </div>
    );
  }, [cursorF1, cursorF2, sampleRate, fftSize, spectrumData]);

  return (
    <div style={{ display: "flex", flexDirection: "column", gap: 10 }}>
      <div style={{ fontSize: 9, fontFamily: T.font, color: T.textMuted }}>
        Click spectrogram: f1 · Shift+Click: f2
      </div>
      <CursorCard
        label="f1 Cursor"
        color={T.pink}
        freq={cursorF1}
        data={spectrumData}
        sampleRate={sampleRate}
        fftSize={fftSize}
      />
      <CursorCard
        label="f2 Cursor"
        color={T.purple}
        freq={cursorF2}
        data={spectrumData}
        sampleRate={sampleRate}
        fftSize={fftSize}
      />
      {deltaBlock}
      <div style={{ display: "flex", gap: 4 }}>
        <Btn onClick={onClearF1} color={T.pink} style={{ flex: 1 }}>
          Clear f1
        </Btn>
        <Btn onClick={onClearF2} color={T.purple} style={{ flex: 1 }}>
          Clear f2
        </Btn>
        <Btn onClick={onClearAll} style={{ flex: 1 }}>
          Clear All
        </Btn>
      </div>
    </div>
  );
}
