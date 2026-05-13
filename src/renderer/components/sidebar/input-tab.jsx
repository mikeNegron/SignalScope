import { T } from "../../lib/tokens.js";
import { Sel, Btn, Section, NumInput } from "../controls.jsx";

// Inline panel that appears under the Source dropdown when "Listen" is
// selected. The user gives the publisher's port + a fallback format and
// sample-rate that the backend uses when the wire header is absent.
function ListenPanel({
  port, format, sampleRate, centerFreq, expectHeader, status,
  onPort, onFormat, onSampleRate, onCenterFreq, onExpectHeader,
}) {
  const statusColor =
    status === "connected" ? T.ok || "#5ad97a" :
    status === "listening" ? T.primary :
    status === "error"     ? T.err || "#e26060" :
                             T.textMuted;
  const statusLabel =
    status === "connected" ? "Receiving" :
    status === "listening" ? "Listening" :
    status === "error"     ? "Error"     :
                             "Stopped";
  return (
    <div style={{ display: "flex", flexDirection: "column", gap: 6, marginTop: 6 }}>
      <NumInput label="Port"   value={port}       onChange={onPort}       min={1}    max={65535} step={1} width={70} />
      <Sel
        label="Format (default)"
        value={String(format)}
        onChange={(v) => onFormat(parseInt(v, 10))}
        options={[
          { value: "0", label: "u8 IQ (RTL-SDR)" },
          { value: "1", label: "i16 IQ" },
          { value: "2", label: "f32 IQ" },
          { value: "3", label: "f32 real" },
        ]}
      />
      <NumInput label="SR (default)" value={sampleRate} onChange={onSampleRate} min={1} max={3e9} step={1} suffix="Hz" width={90} />
      <NumInput label="CF (default)" value={centerFreq} onChange={onCenterFreq} min={0} max={4.29e9} step={1} suffix="Hz" width={90} />
      <Btn
        active={expectHeader}
        onClick={() => onExpectHeader(!expectHeader)}
        title="When on, the publisher must send a 16-byte SS01 header. When off, raw bytes are decoded with the defaults above."
      >
        {expectHeader ? "Expect header: ON" : "Expect header: OFF"}
      </Btn>
      <div style={{
        display: "flex", alignItems: "center", gap: 6,
        fontSize: 9, fontFamily: T.font, color: statusColor, marginTop: 2,
      }}>
        <div style={{ width: 6, height: 6, borderRadius: "50%", background: statusColor }} />
        {statusLabel}
      </div>
    </div>
  );
}

export function InputTab({
  backendConnected,
  sampleRate,
  onSampleRate,
  // The active source label. Owned by the parent and persisted there
  // so navigating away from this tab doesn't reset the selection.
  source = "websocket",
  onSource,
  onOpenFile,
  onReconnect,
  connColor,
  // File replay speed multiplier (1.0 = real-time). Only meaningful
  // when the file source is active; backend ignores it otherwise.
  replaySpeed = 1.0,
  onReplaySpeed,
  // Listen-source state. Wire format is documented in
  // src/backend/source/iq_listener.hpp.
  listenPort = 5555,
  listenFormat = 2,            // 0=U8IQ, 1=I16IQ, 2=F32IQ, 3=F32Real
  listenSampleRate = 48000,
  listenCenterFreq = 0,
  listenExpectHeader = true,
  listenStatus = "stopped",     // 'stopped' | 'listening' | 'connected' | 'error'
  onListenPort,
  onListenFormat,
  onListenSampleRate,
  onListenCenterFreq,
  onListenExpectHeader,
}) {
  return (
    <div style={{ display: "flex", flexDirection: "column", gap: 12 }}>
      <Section label="Source">
        <Sel
          label=""
          value={source}
          onChange={(v) => onSource?.(v)}
          options={[
            { value: "websocket", label: "WebSocket (C++ Backend)" },
            { value: "mic", label: "Microphone" },
            { value: "test", label: "Test Signal" },
            { value: "file", label: "File Input" },
            { value: "listen", label: "Listen (TCP)" },
            ...(!backendConnected ? [{ value: "simulated", label: "Simulated" }] : []),
          ]}
        />
        {source === "file" && (
          <>
            <Btn
              onClick={onOpenFile}
              active
              style={{ marginTop: 6, width: "100%", justifyContent: "center" }}
            >
              Browse…
            </Btn>
            <div style={{ marginTop: 6 }}>
              <NumInput
                label="Replay speed"
                value={replaySpeed}
                onChange={onReplaySpeed}
                min={0.1}
                max={32}
                step={0.1}
                suffix="×"
                width={60}
              />
              <div style={{ fontSize: 9, color: T.textMuted, marginTop: 4 }}>
                Multiplies the file's natural sample rate. Higher = more
                FFTs per wall-clock second at large fft_size.
              </div>
            </div>
          </>
        )}
        {source === "listen" && (
          <ListenPanel
            port={listenPort}
            format={listenFormat}
            sampleRate={listenSampleRate}
            centerFreq={listenCenterFreq}
            expectHeader={listenExpectHeader}
            status={listenStatus}
            onPort={onListenPort}
            onFormat={onListenFormat}
            onSampleRate={onListenSampleRate}
            onCenterFreq={onListenCenterFreq}
            onExpectHeader={onListenExpectHeader}
          />
        )}
      </Section>
      <Section label="Sample Rate">
        <NumInput
          value={sampleRate}
          onChange={onSampleRate}
          min={1}
          max={3000000000}
          step={1}
          suffix="Hz"
          width={90}
        />
      </Section>
      <div
        style={{
          background: T.bgElev,
          border: `1px solid ${connColor}33`,
          borderRadius: 4,
          padding: "6px 8px",
          fontFamily: T.font,
          fontSize: 10,
        }}
      >
        <div style={{ color: T.textMid, marginBottom: 4 }}>ws://localhost:8765</div>
        <div
          style={{
            display: "flex",
            alignItems: "center",
            justifyContent: "space-between",
          }}
        >
          <div style={{ display: "flex", alignItems: "center", gap: 4 }}>
            <div
              style={{
                width: 6,
                height: 6,
                borderRadius: "50%",
                background: connColor,
              }}
            />
            <span style={{ color: connColor, fontSize: 9 }}>
              {backendConnected ? "Connected" : "Disconnected"}
            </span>
          </div>
          {!backendConnected && onReconnect && (
            <button
              onClick={onReconnect}
              style={{
                background: T.primaryDim + "33",
                color: T.primary,
                border: `1px solid ${T.primaryDim}`,
                borderRadius: 3,
                padding: "1px 6px",
                fontSize: 9,
                fontFamily: T.font,
                cursor: "pointer",
              }}
            >
              Reconnect
            </button>
          )}
        </div>
      </div>
    </div>
  );
}
