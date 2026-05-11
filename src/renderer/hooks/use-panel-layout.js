import { useState, useEffect, useRef, useCallback } from "react";

export const PANEL_KEYS = ["spectrogram", "spectrum", "waveform", "iq"];

export function usePanelLayout() {
  const [panels, setPanels] = useState({
    spectrogram: true,
    spectrum: true,
    waveform: false,
    iq: true,
    sidebar: true,
  });
  const tog = useCallback((k) => setPanels((p) => ({ ...p, [k]: !p[k] })), []);

  const [pFracs, setPFracs] = useState({
    spectrogram: 0.4,
    spectrum: 0.28,
    waveform: 0.18,
    iq: 0.14,
  });

  useEffect(() => {
    const vis = PANEL_KEYS.filter((k) => panels[k]);
    if (!vis.length) return;
    const tot = vis.reduce((s, k) => s + (pFracs[k] || 0.25), 0);
    const nxt = { ...pFracs };
    vis.forEach((k) => (nxt[k] = (nxt[k] || 0.25) / tot));
    setPFracs(nxt);
  }, [panels.spectrogram, panels.spectrum, panels.waveform, panels.iq]);

  const containerRef = useRef(null);
  const dragR = useRef(null);
  const onDiv = useCallback(
    (kA, kB, e) => {
      e.preventDefault();
      dragR.current = { kA, kB, y0: e.clientY, sf: { ...pFracs } };
      const mv = (me) => {
        if (!dragR.current || !containerRef.current) return;
        const { kA, kB, y0, sf } = dragR.current;
        const d =
          (me.clientY - y0) /
          containerRef.current.getBoundingClientRect().height;
        setPFracs((p) => ({
          ...p,
          [kA]: Math.max(0.05, sf[kA] + d),
          [kB]: Math.max(0.05, sf[kB] - d),
        }));
      };
      const up = () => {
        dragR.current = null;
        window.removeEventListener("mousemove", mv);
        window.removeEventListener("mouseup", up);
      };
      window.addEventListener("mousemove", mv);
      window.addEventListener("mouseup", up);
    },
    [pFracs]
  );

  const [sbW, setSbW] = useState(260);
  const sbDrag = useRef(null);
  const onSbDrag = useCallback(
    (e) => {
      e.preventDefault();
      sbDrag.current = { x0: e.clientX, w0: sbW };
      const mv = (me) => {
        if (!sbDrag.current) return;
        setSbW(
          Math.max(
            180,
            Math.min(500, sbDrag.current.w0 + (sbDrag.current.x0 - me.clientX))
          )
        );
      };
      const up = () => {
        sbDrag.current = null;
        window.removeEventListener("mousemove", mv);
        window.removeEventListener("mouseup", up);
      };
      window.addEventListener("mousemove", mv);
      window.addEventListener("mouseup", up);
    },
    [sbW]
  );

  const visKeys = PANEL_KEYS.filter((k) => panels[k]);
  const hidKeys = PANEL_KEYS.filter((k) => !panels[k]);

  return { panels, tog, pFracs, containerRef, onDiv, sbW, onSbDrag, visKeys, hidKeys };
}
