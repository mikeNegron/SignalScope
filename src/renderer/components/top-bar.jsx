import { T } from "../lib/tokens.js";
import { Btn } from "./controls.jsx";

export function TopBar({
  mode,
  onMode,
  backendConnected,
  sampleRate,
  fftSize,
  srcLabel,
  connColor,
}) {
  return (
    <div
      style={{
        display: "flex",
        alignItems: "center",
        justifyContent: "space-between",
        padding: "0 12px",
        height: 36,
        borderBottom: `1px solid ${T.border}`,
        background: T.bgPanel,
        flexShrink: 0,
      }}
    >
      <div style={{ display: "flex", alignItems: "center", gap: 10 }}>
        <div
          style={{
            width: 22,
            height: 22,
            borderRadius: 4,
            background: `linear-gradient(135deg,${T.primary},${T.purple})`,
            display: "flex",
            alignItems: "center",
            justifyContent: "center",
            fontSize: 11,
            fontWeight: 800,
            color: T.bg,
          }}
        >
          S
        </div>
        <span
          style={{
            fontFamily: T.font,
            fontSize: 13,
            fontWeight: 700,
            letterSpacing: "-.02em",
          }}
        >
          SignalScope
        </span>
        <span style={{ fontFamily: T.font, fontSize: 10, color: T.textMuted }}>
          v0.3.0
        </span>
        {!backendConnected && (
          <span
            style={{
              fontFamily: T.font,
              fontSize: 9,
              color: T.orange,
              background: T.orange + "15",
              padding: "1px 6px",
              borderRadius: 3,
              border: `1px solid ${T.orange}33`,
            }}
          >
            DEMO
          </span>
        )}
      </div>

      <div style={{ display: "flex", alignItems: "center", gap: 4 }}>
        <Btn active={mode === "record"} onClick={() => onMode("record")} color={T.error}>
          <span
            style={{
              width: 8,
              height: 8,
              borderRadius: "50%",
              background: mode === "record" ? T.error : T.textMuted,
              display: "inline-block",
            }}
          />{" "}
          REC
        </Btn>
        <Btn active={mode === "pause"} onClick={() => onMode("pause")}>
          ⏸ PAUSE
        </Btn>
        <Btn active={mode === "play"} onClick={() => onMode("play")} color={T.success}>
          ▶ PLAY
        </Btn>
        <div style={{ width: 1, height: 16, background: T.border, margin: "0 6px" }} />
        <div
          style={{
            width: 7,
            height: 7,
            borderRadius: "50%",
            background:
              mode === "record" ? T.error : mode === "play" ? T.success : T.textMuted,
            boxShadow: `0 0 6px ${
              mode === "record" ? T.error : mode === "play" ? T.success : T.textMuted
            }66`,
          }}
        />
        <span style={{ fontFamily: T.font, fontSize: 10, color: T.textMid }}>
          {mode === "record" ? "Recording" : mode === "play" ? "Playing" : "Paused"}
        </span>
      </div>

      <div style={{ display: "flex", alignItems: "center", gap: 14 }}>
        <span style={{ fontFamily: T.font, fontSize: 10, color: T.textMuted }}>
          SR:{" "}
          <span style={{ color: T.textMid }}>{(sampleRate / 1000).toFixed(1)}k</span>
        </span>
        <span style={{ fontFamily: T.font, fontSize: 10, color: T.textMuted }}>
          FFT: <span style={{ color: T.textMid }}>{fftSize}</span>
        </span>
        <span style={{ fontFamily: T.font, fontSize: 10, color: T.textMuted }}>
          BIN:{" "}
          <span style={{ color: T.textMid }}>
            {(sampleRate / +fftSize).toFixed(2)}Hz
          </span>
        </span>
        <span style={{ fontFamily: T.font, fontSize: 10, color: T.textMuted }}>
          WS: <span style={{ color: connColor }}>●</span>{" "}
          <span style={{ color: T.textMid }}>{srcLabel}</span>
        </span>
      </div>
    </div>
  );
}
