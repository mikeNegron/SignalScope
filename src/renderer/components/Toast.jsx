import { useEffect, useRef } from "react";
import { T } from "../lib/tokens.js";

// Floating status toast pinned to the bottom-right of the viewport.
// Three kinds:
//   - "pending": sticky; caller swaps to success/error when async work resolves.
//   - "success": auto-dismisses after 4s.
//   - "error":   auto-dismisses after 4s.
// Parent owns the { kind, msg } state and renders <Toast /> when non-null.
// onDismiss is called when the user clicks the × OR when the auto-timer fires;
// the parent should clear its state in response.
const AUTO_DISMISS_MS = 4000;

const COLOR_BY_KIND = {
  pending: { bg: T.bgElev, border: T.border,  text: T.text    },
  success: { bg: T.bgElev, border: T.success, text: T.success },
  error:   { bg: T.bgElev, border: T.error,   text: T.error   },
};

export function Toast({ kind, msg, onDismiss }) {
  // Latch onDismiss in a ref so the auto-dismiss timer doesn't reset
  // when a parent passes an unmemoized callback. Without this, a parent
  // that re-renders faster than AUTO_DISMISS_MS could indefinitely
  // postpone auto-dismiss.
  const onDismissRef = useRef(onDismiss);
  useEffect(() => { onDismissRef.current = onDismiss; }, [onDismiss]);

  useEffect(() => {
    if (kind === "pending") return;
    const t = setTimeout(() => onDismissRef.current(), AUTO_DISMISS_MS);
    return () => clearTimeout(t);
  }, [kind, msg]);

  const c = COLOR_BY_KIND[kind] || COLOR_BY_KIND.pending;

  return (
    <div
      data-testid={`toast-${kind}`}
      role={kind === "error" ? "alert" : "status"}
      style={{
        position: "fixed",
        right: 16,
        bottom: 16,
        background: c.bg,
        border: `1px solid ${c.border}`,
        color: c.text,
        padding: "8px 12px",
        borderRadius: 4,
        fontFamily: T.font,
        fontSize: 12,
        maxWidth: 360,
        display: "flex",
        alignItems: "center",
        gap: 8,
        zIndex: 1000,
        boxShadow: "0 4px 12px rgba(0,0,0,0.3)",
      }}
    >
      {kind === "pending" && (
        <span style={{
          display: "inline-block",
          width: 12,
          height: 12,
          border: `2px solid ${T.border}`,
          borderTopColor: c.text,
          borderRadius: "50%",
          animation: "spin 0.8s linear infinite",
        }} />
      )}
      <span style={{ flex: 1 }}>{msg}</span>
      <button
        onClick={onDismiss}
        aria-label="Dismiss"
        style={{
          background: "none",
          border: "none",
          color: c.text,
          cursor: "pointer",
          fontSize: 14,
          padding: 0,
          lineHeight: 1,
        }}
      >
        ×
      </button>
    </div>
  );
}
