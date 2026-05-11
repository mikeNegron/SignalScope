import { useEffect, useState } from "react";
import { T, safeSpecRead } from "../lib/tokens.js";

// Plain numeric input with the spinner arrows hidden (rules in global.css).
// Commits on Enter or blur - typing partial values like "48" on the way to
// "48000" does not fire onChange. Reverts to the canonical `value` if the
// field is left blank or invalid.
export function NumInput({ label, value, onChange, min, max, step, suffix, width = 70 }) {
  const [draft, setDraft] = useState(String(value ?? ""));
  // Re-sync the draft whenever the upstream value changes (e.g. backend
  // pushes a new sample rate after a file load).
  useEffect(() => { setDraft(String(value ?? "")); }, [value]);

  const commit = () => {
    const n = Number(draft);
    if (!Number.isFinite(n)) { setDraft(String(value)); return; }
    let v = n;
    if (min != null) v = Math.max(min, v);
    if (max != null) v = Math.min(max, v);
    if (step && step >= 1) v = Math.round(v);
    setDraft(String(v));
    if (v !== value) onChange(v);
  };

  return (
    <label
      style={{
        display: "flex", alignItems: "center", gap: 5,
        fontSize: 10, fontFamily: T.font, color: T.textMuted,
      }}
    >
      {label}
      <input
        type="number"
        value={draft}
        min={min}
        max={max}
        step={step}
        onChange={(e) => setDraft(e.target.value)}
        onBlur={commit}
        onKeyDown={(e) => { if (e.key === "Enter") e.currentTarget.blur(); }}
        style={{
          width,
          background: T.bgElev,
          color: T.text,
          border: `1px solid ${T.border}`,
          borderRadius: 3,
          padding: "2px 6px",
          fontSize: 10,
          fontFamily: T.font,
          outline: "none",
          textAlign: "right",
        }}
      />
      {suffix && (
        <span style={{ color: T.textMid, fontSize: 9 }}>{suffix}</span>
      )}
    </label>
  );
}

export function Sel({ label, value, onChange, options }) {
  return (
    <label
      style={{
        display: "flex",
        alignItems: "center",
        gap: 5,
        fontSize: 10,
        fontFamily: T.font,
        color: T.textMuted,
      }}
    >
      {label}
      <select
        value={value}
        onChange={(e) => onChange(e.target.value)}
        style={{
          background: T.bgElev,
          color: T.text,
          border: `1px solid ${T.border}`,
          borderRadius: 3,
          padding: "2px 4px",
          fontSize: 10,
          fontFamily: T.font,
          cursor: "pointer",
          outline: "none",
        }}
      >
        {options.map((o) => (
          <option
            key={typeof o === "string" ? o : o.value}
            value={typeof o === "string" ? o : o.value}
          >
            {typeof o === "string" ? o : o.label}
          </option>
        ))}
      </select>
    </label>
  );
}

export function Knob({
  label,
  value,
  onChange,
  min = 0,
  max = 100,
  unit = "",
  numeric = false,
}) {
  return (
    <label
      style={{
        display: "flex",
        alignItems: "center",
        gap: 4,
        fontSize: 10,
        fontFamily: T.font,
        color: T.textMuted,
      }}
    >
      {label}
      {numeric ? (
        <input
          type="number"
          min={min}
          max={max}
          value={value}
          onChange={(e) => {
            const v = Math.max(min, Math.min(max, +e.target.value));
            onChange(v);
          }}
          style={{
            width: 48,
            background: T.bgElev,
            color: T.text,
            border: `1px solid ${T.border}`,
            borderRadius: 3,
            padding: "2px 4px",
            fontSize: 10,
            fontFamily: T.font,
            outline: "none",
            textAlign: "right",
          }}
        />
      ) : (
        <input
          type="range"
          min={min}
          max={max}
          value={value}
          onChange={(e) => onChange(+e.target.value)}
          style={{
            width: 50,
            accentColor: T.primary,
            height: 3,
            cursor: "pointer",
          }}
        />
      )}
      <span
        style={{
          color: T.textMid,
          minWidth: numeric ? 16 : 30,
          textAlign: "right",
          fontSize: 9,
        }}
      >
        {numeric ? unit : `${value}${unit}`}
      </span>
    </label>
  );
}

export function Btn({ children, active, onClick, color, style: s, title: ti }) {
  return (
    <button
      onClick={onClick}
      title={ti}
      style={{
        background: active ? (color || T.primaryDim) + "33" : "transparent",
        color: active ? color || T.primary : T.textMuted,
        border: `1px solid ${active ? color || T.primaryDim : T.border}`,
        borderRadius: 3,
        padding: "2px 8px",
        fontSize: 10,
        fontFamily: T.font,
        cursor: "pointer",
        display: "flex",
        alignItems: "center",
        gap: 4,
        transition: "all 120ms",
        whiteSpace: "nowrap",
        ...s,
      }}
    >
      {children}
    </button>
  );
}

export function MRow({ label, value, unit, color }) {
  return (
    <div
      style={{
        display: "flex",
        justifyContent: "space-between",
        alignItems: "baseline",
        padding: "2px 0",
        borderBottom: `1px solid ${T.borderSub}`,
      }}
    >
      <span style={{ color: T.textMuted, fontSize: 10, fontFamily: T.font }}>
        {label}
      </span>
      <span
        style={{
          color: color || T.text,
          fontSize: 11,
          fontFamily: T.font,
          fontWeight: 600,
        }}
      >
        {value}{" "}
        <span style={{ color: T.textMuted, fontSize: 9, fontWeight: 400 }}>
          {unit}
        </span>
      </span>
    </div>
  );
}

export function Section({ label, children, color }) {
  return (
    <div>
      <div
        style={{
          fontSize: 9,
          fontFamily: T.font,
          color: color || T.textMuted,
          textTransform: "uppercase",
          marginBottom: 4,
          letterSpacing: ".1em",
        }}
      >
        {label}
      </div>
      {children}
    </div>
  );
}

export function CursorCard({ label, color, freq, data, sampleRate, fftSize }) {
  if (freq == null || !isFinite(freq))
    return (
      <div
        style={{
          background: T.bgElev,
          border: `1px solid ${T.border}`,
          borderRadius: 4,
          padding: "8px 10px",
        }}
      >
        <div
          style={{
            fontSize: 9,
            fontFamily: T.font,
            color,
            textTransform: "uppercase",
            marginBottom: 6,
            letterSpacing: ".1em",
            display: "flex",
            alignItems: "center",
            gap: 6,
          }}
        >
          <span
            style={{
              width: 8,
              height: 8,
              borderRadius: "50%",
              background: T.border,
              display: "inline-block",
            }}
          />
          {label}
        </div>
        <div
          style={{
            fontFamily: T.font,
            fontSize: 10,
            color: T.textMuted,
            padding: "4px 0",
          }}
        >
          Not set
        </div>
      </div>
    );

  const binRes = sampleRate / +fftSize;
  return (
    <div
      style={{
        background: T.bgElev,
        border: `1px solid ${T.border}`,
        borderRadius: 4,
        padding: "8px 10px",
      }}
    >
      <div
        style={{
          fontSize: 9,
          fontFamily: T.font,
          color,
          textTransform: "uppercase",
          marginBottom: 6,
          letterSpacing: ".1em",
          display: "flex",
          alignItems: "center",
          gap: 6,
        }}
      >
        <span
          style={{
            width: 8,
            height: 8,
            borderRadius: "50%",
            background: color,
            display: "inline-block",
          }}
        />
        {label}
      </div>
      <MRow label="Frequency" value={freq.toFixed(3)} unit="Hz" color={color} />
      <MRow label="kHz" value={(freq / 1000).toFixed(5)} unit="kHz" />
      <MRow
        label="Bin #"
        value={binRes > 0 ? Math.round(freq / binRes) : 0}
        unit=""
      />
      <MRow
        label="Level"
        value={safeSpecRead(data, freq, sampleRate).toFixed(1)}
        unit="dBFS"
      />
    </div>
  );
}
