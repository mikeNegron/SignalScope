import { T } from "../lib/tokens.js";
import { DragHandle } from "./panels.jsx";

const PANEL_LABELS = {
  spectrogram: "Spectrogram",
  spectrum: "Spectrum",
  waveform: "Waveform",
  iq: "IQ Constellation",
};

export function PanelGrid({
  tog,
  pFracs,
  containerRef,
  onDiv,
  visKeys,
  hidKeys,
  slots,
}) {
  return (
    <div
      ref={containerRef}
      style={{
        flex: 1,
        display: "flex",
        flexDirection: "column",
        overflow: "hidden",
        minWidth: 0,
      }}
    >
      {hidKeys.map((k) => (
        <div
          key={k}
          onClick={() => tog(k)}
          style={{
            background: T.bgElev,
            borderBottom: `1px solid ${T.borderSub}`,
            padding: "1px 10px",
            display: "flex",
            alignItems: "center",
            justifyContent: "space-between",
            cursor: "pointer",
            flexShrink: 0,
            height: 20,
          }}
        >
          <span
            style={{
              fontFamily: T.font,
              fontSize: 9,
              color: T.textMuted,
              textTransform: "uppercase",
            }}
          >
            {PANEL_LABELS[k]}
          </span>
          <span style={{ fontFamily: T.font, fontSize: 9, color: T.primaryDim }}>
            ▸ Show
          </span>
        </div>
      ))}
      {visKeys.map((k, i) => {
        const tot = visKeys.reduce((s, v) => s + (pFracs[v] || 0.25), 0);
        return (
          <div
            key={k}
            style={{
              display: "flex",
              flexDirection: "column",
              overflow: "hidden",
              flex: `0 0 ${((pFracs[k] || 0.25) / tot) * 100}%`,
              minHeight: 30,
            }}
          >
            {i > 0 && (
              <DragHandle
                onMouseDown={(e) => onDiv(visKeys[i - 1], k, e)}
                direction="vertical"
              />
            )}
            {slots[k]}
          </div>
        );
      })}
    </div>
  );
}
