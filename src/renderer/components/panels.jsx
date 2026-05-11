import { useState, useEffect, useRef } from "react";
import { T } from "../lib/tokens.js";

export function Panel({ title, children, toolbar, onToggle }) {
  return (
    <div
      style={{
        background: T.bgPanel,
        display: "flex",
        flexDirection: "column",
        overflow: "hidden",
        minWidth: 0,
        minHeight: 0,
        height: "100%",
      }}
    >
      <div
        style={{
          display: "flex",
          alignItems: "center",
          justifyContent: "space-between",
          padding: "4px 10px",
          borderBottom: `1px solid ${T.borderSub}`,
          background: T.bgElev,
          flexShrink: 0,
        }}
      >
        <span
          style={{
            fontFamily: T.font,
            fontSize: 10,
            fontWeight: 600,
            color: T.textMuted,
            textTransform: "uppercase",
            letterSpacing: ".08em",
            display: "flex",
            alignItems: "center",
            gap: 8,
          }}
        >
          {title}
        </span>
        <div style={{ display: "flex", gap: 6, alignItems: "center" }}>
          {toolbar}
          {onToggle && (
            <button
              onClick={onToggle}
              style={{
                background: "none",
                border: "none",
                color: T.textMuted,
                cursor: "pointer",
                fontFamily: T.font,
                fontSize: 10,
                padding: "0 4px",
                lineHeight: 1,
              }}
            >
              ✕
            </button>
          )}
        </div>
      </div>
      <div style={{ flex: 1, position: "relative", overflow: "hidden" }}>
        {children}
      </div>
    </div>
  );
}

export function DragHandle({ onMouseDown, direction = "vertical" }) {
  const [h, setH] = useState(false);
  const isV = direction === "vertical";
  return (
    <div
      onMouseDown={onMouseDown}
      onMouseEnter={() => setH(true)}
      onMouseLeave={() => setH(false)}
      style={{
        [isV ? "height" : "width"]: 6,
        [isV ? "width" : "height"]: "100%",
        cursor: isV ? "row-resize" : "col-resize",
        background: h ? T.primaryDim + "44" : "transparent",
        display: "flex",
        alignItems: "center",
        justifyContent: "center",
        flexShrink: 0,
        zIndex: 10,
      }}
    >
      <div
        style={{
          [isV ? "width" : "height"]: 30,
          [isV ? "height" : "width"]: 2,
          borderRadius: 1,
          background: h ? T.primary : T.border,
        }}
      />
    </div>
  );
}

export function useCanvasSize(ref) {
  const [d, setD] = useState({ w: 512, h: 200 });
  useEffect(() => {
    if (!ref.current) return;
    const ro = new ResizeObserver(([e]) => {
      const { width: w, height: h } = e.contentRect;
      if (w > 0 && h > 0) setD({ w: Math.round(w), h: Math.round(h) });
    });
    ro.observe(ref.current);
    return () => ro.disconnect();
  }, []);
  return d;
}
