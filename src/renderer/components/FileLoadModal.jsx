import { useState, useEffect } from "react";
import { T } from "../lib/tokens.js";

// SigMF datatype options. Built from the cross product of:
//   prefix: r (real) | c (complex)
//   sample type: f32, f64, i32, i16, i8, u8
//   endian: _le, _be
// Listed in rough order of "most common first".
const DATATYPES = [
  "rf32_le", "cf32_le",
  "ri16_le", "ci16_le",
  "rf64_le", "cf64_le",
  "ri32_le", "ci32_le",
  "ri8",     "ci8",
  "ru8",     "cu8",
  "rf32_be", "cf32_be",
  "ri16_be", "ci16_be",
];

const SAMPLE_RATES = [8000, 16000, 22050, 32000, 44100, 48000, 96000, 192000];

// Modal: prompts the user for the parameters needed to interpret a non-self-
// describing signal file (.raw, .f32, or .sigmf-data without a sidecar).
// Pre-fills from `hint` when supplied (e.g. a sigmf-meta we managed to parse).
//
// Calls `onSubmit({ datatype, sample_rate, channels })` on confirm.
// Calls `onCancel()` on dismiss.
export function FileLoadModal({ filePath, format, hint, onSubmit, onCancel }) {
  const [datatype, setDatatype] = useState(hint?.datatype || "");
  const [sampleRate, setSampleRate] = useState(hint?.sample_rate || "");
  const [channels, setChannels] = useState(hint?.channels || 1);

  useEffect(() => {
    setDatatype(hint?.datatype || "");
    setSampleRate(hint?.sample_rate || "");
    setChannels(hint?.channels || 1);
  }, [hint]);

  const valid = datatype && Number(sampleRate) > 0 && Number(channels) > 0;

  const fileName = filePath ? filePath.split(/[\\/]/).pop() : "";

  return (
    <div
      onClick={onCancel}
      style={{
        position: "fixed", inset: 0,
        background: "rgba(0,0,0,0.55)",
        display: "flex", alignItems: "center", justifyContent: "center",
        zIndex: 1000,
      }}
    >
      <div
        onClick={(e) => e.stopPropagation()}
        style={{
          width: 420,
          background: T.bgPanel,
          border: `1px solid ${T.border}`,
          borderRadius: 6,
          padding: 18,
          fontFamily: T.font,
          fontSize: 11,
          color: T.text,
          boxShadow: "0 12px 32px rgba(0,0,0,0.5)",
        }}
      >
        <div style={{
          fontSize: 11, fontWeight: 700, color: T.text,
          textTransform: "uppercase", letterSpacing: ".08em",
          marginBottom: 4,
        }}>
          File parameters needed
        </div>
        <div style={{ fontSize: 10, color: T.textMid, marginBottom: 14 }}>
          {format === "sigmf"
            ? `No .sigmf-meta sidecar found for ${fileName}.`
            : `${fileName} has no embedded format header.`}
          {" "}Fill in the values below to decode it.
        </div>

        <Field label="Sample rate (Hz)">
          <input
            type="number"
            min={1}
            value={sampleRate}
            onChange={(e) => setSampleRate(e.target.value)}
            list="rate-suggestions"
            placeholder="e.g. 48000"
            style={inputStyle}
          />
          <datalist id="rate-suggestions">
            {SAMPLE_RATES.map((r) => <option key={r} value={r} />)}
          </datalist>
        </Field>

        <Field label="Datatype (SigMF style)">
          <select
            value={datatype}
            onChange={(e) => setDatatype(e.target.value)}
            style={inputStyle}
          >
            <option value="">- select -</option>
            {DATATYPES.map((d) => (
              <option key={d} value={d}>{labelForDatatype(d)}</option>
            ))}
          </select>
        </Field>

        <Field label="Channels">
          <input
            type="number"
            min={1}
            max={16}
            value={channels}
            onChange={(e) => setChannels(e.target.value)}
            style={inputStyle}
          />
        </Field>

        <div style={{
          fontSize: 9, color: T.textMuted,
          marginTop: 6, marginBottom: 14,
        }}>
          {datatype && (datatype.startsWith("c")
            ? "Complex / IQ - feeds the two-sided spectrum and the IQ scatter directly."
            : "Real - feeds the one-sided spectrum and the waveform.")}
        </div>

        <div style={{ display: "flex", gap: 8, justifyContent: "flex-end" }}>
          <button
            onClick={onCancel}
            style={btnStyle(false)}
          >
            Cancel
          </button>
          <button
            onClick={() => valid && onSubmit({
              datatype,
              sample_rate: Number(sampleRate),
              channels: Number(channels),
            })}
            disabled={!valid}
            style={btnStyle(true, !valid)}
          >
            Load
          </button>
        </div>
      </div>
    </div>
  );
}

// Human-readable label, e.g. "rf32_le -> real float32 little-endian"
function labelForDatatype(d) {
  const m = d.match(/^([rc])([fiu])(\d+)(?:_(le|be))?$/);
  if (!m) return d;
  const [, rc, fi, bits, en] = m;
  const kind = rc === "c" ? "complex" : "real";
  const stype = fi === "f" ? "float" : (fi === "i" ? "int" : "uint");
  const endian = en ? (en === "le" ? "LE" : "BE") : "";
  return `${d}  ·  ${kind} ${stype}${bits}${endian ? " " + endian : ""}`;
}

function Field({ label, children }) {
  return (
    <div style={{ marginBottom: 10 }}>
      <div style={{
        fontSize: 9, color: T.textMid,
        textTransform: "uppercase", letterSpacing: ".08em",
        marginBottom: 4,
      }}>
        {label}
      </div>
      {children}
    </div>
  );
}

const inputStyle = {
  width: "100%",
  background: T.bgElev,
  color: T.text,
  border: `1px solid ${T.border}`,
  borderRadius: 4,
  padding: "6px 8px",
  fontFamily: T.font,
  fontSize: 11,
  outline: "none",
};

function btnStyle(primary, disabled = false) {
  return {
    padding: "6px 14px",
    fontSize: 10,
    fontFamily: T.font,
    fontWeight: 600,
    textTransform: "uppercase",
    letterSpacing: ".08em",
    cursor: disabled ? "not-allowed" : "pointer",
    background: primary ? T.primary : T.bgElev,
    color: primary ? "#0b1416" : T.text,
    border: `1px solid ${primary ? T.primary : T.border}`,
    borderRadius: 4,
    opacity: disabled ? 0.5 : 1,
  };
}
