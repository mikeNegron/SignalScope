import { T } from "../lib/tokens.js";

export function StatusBar({
  cursorF1,
  sampleRate,
  fftSize,
  backendConnected,
  srcLabel,
  backendFrameId,
  timeRef,
  fileLooping = false,
  loopCount = 0,
}) {
  return (
    <div
      style={{
        display: "flex",
        alignItems: "center",
        justifyContent: "space-between",
        padding: "0 12px",
        height: 20,
        borderTop: `1px solid ${T.border}`,
        background: T.bgPanel,
        flexShrink: 0,
      }}
    >
      <div style={{ display: "flex", gap: 16 }}>
        <span style={{ fontFamily: T.font, fontSize: 9, color: T.textMuted }}>
          Cursor:{" "}
          <span style={{ color: T.textMid }}>
            {cursorF1 != null ? `${cursorF1.toFixed(1)} Hz` : "-"}
          </span>
        </span>
        <span style={{ fontFamily: T.font, fontSize: 9, color: T.textMuted }}>
          Bin:{" "}
          <span data-testid="status-sample-rate" style={{ color: T.textMid }}>
            {(sampleRate / +fftSize).toFixed(2)} Hz
          </span>
        </span>
        <span style={{ fontFamily: T.font, fontSize: 9, color: T.textMuted }}>
          Src:{" "}
          <span data-testid="status-source-label" style={{ color: backendConnected ? T.success : T.orange }}>
            {srcLabel}
          </span>
        </span>
        {fileLooping && (
          <span style={{ fontFamily: T.font, fontSize: 9, color: T.textMuted }}>
            Loop:{" "}
            <span style={{ color: T.info }}>
              ↻ {loopCount}
            </span>
          </span>
        )}
      </div>
      <div style={{ display: "flex", gap: 16 }}>
        <span style={{ fontFamily: T.font, fontSize: 9, color: T.textMuted }}>
          UTC:{" "}
          <span style={{ color: T.textMid }}>
            {new Date().toISOString().slice(11, 23)}
          </span>
        </span>
        <span style={{ fontFamily: T.font, fontSize: 9, color: T.textMuted }}>
          Frame:{" "}
          <span data-testid="status-frame-id" style={{ color: T.textMid }}>
            {backendConnected ? backendFrameId : Math.floor(timeRef.current * 20)}
          </span>
        </span>
      </div>
    </div>
  );
}
