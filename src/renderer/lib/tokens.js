export const T = {
  bg: "#09090B",
  bgPanel: "#0F0F11",
  bgElev: "#161618",
  bgHover: "#1C1C1F",
  border: "#27272A",
  borderSub: "#1E1E21",
  text: "#ECEDEE",
  textMid: "#A1A1AA",
  textMuted: "#52525B",
  primary: "#22D3EE",
  primaryDim: "#0E7490",
  success: "#4ADE80",
  warning: "#FACC15",
  error: "#F87171",
  info: "#60A5FA",
  purple: "#C084FC",
  orange: "#FB923C",
  pink: "#F472B6",
  font: "'IBM Plex Mono','JetBrains Mono','Fira Code',monospace",
  fontSans: "'IBM Plex Sans','Inter',system-ui,sans-serif",
};

// Per-peak marker palette. Index N = color for peaks[N] (peaks are sorted
// strongest -> weakest by the backend, so peaks[0] gets the brightest /
// most attention-grabbing color). The Peaks sidebar tab uses the same
// palette so circle and table-row colors line up visually.
export const PEAK_PALETTE = [
  "#FACC15", // 0 - yellow   (warning)
  "#FB923C", // 1 - orange
  "#F87171", // 2 - red      (error)
  "#F472B6", // 3 - pink
  "#C084FC", // 4 - purple
  "#60A5FA", // 5 - blue     (info)
  "#22D3EE", // 6 - cyan     (primary)
  "#4ADE80", // 7 - green    (success)
];

export const CMAPS = {
  inferno: (v) => {
    const r = Math.min(
        255,
        v < 0.5 ? (v * 400) | 0 : (200 + (v - 0.5) * 110) | 0
      ),
      g = Math.min(
        255,
        v < 0.4
          ? 0
          : v < 0.7
          ? (((v - 0.4) / 0.3) * 180) | 0
          : (180 + ((v - 0.7) / 0.3) * 75) | 0
      ),
      b = Math.min(
        255,
        v < 0.2 ? (v * 400) | 0 : v < 0.5 ? (80 - (v - 0.2) * 267) | 0 : 0
      );
    return [r, g, b];
  },
  viridis: (v) => [
    (68 + v * 185) | 0,
    v < 0.5 ? (1 + v * 294) | 0 : (148 + (v - 0.5) * 166) | 0,
    v < 0.5 ? (84 + v * 2) | 0 : (85 - (v - 0.5) * 170) | 0,
  ],
  plasma: (v) => [
    v < 0.5 ? (13 + v * 374) | 0 : (200 + (v - 0.5) * 80) | 0,
    v < 0.3 ? 8 : v < 0.7 ? ((v - 0.3) * 500) | 0 : (200 + (v - 0.7) * 183) | 0,
    v < 0.5 ? (135 - v * 100) | 0 : (85 - (v - 0.5) * 170) | 0,
  ],
  baudline: (v) => [
    v > 0.7 ? ((v - 0.7) * 850) | 0 : 0,
    (v * 255) | 0,
    v < 0.3 ? (v * 333) | 0 : (100 - (v - 0.3) * 143) | 0,
  ],
  hot: (v) => [
    Math.min(255, (v * 765) | 0),
    Math.min(255, Math.max(0, ((v - 0.33) * 765) | 0)),
    Math.min(255, Math.max(0, ((v - 0.66) * 765) | 0)),
  ],
  cool: (v) => [(v * 255) | 0, (255 - v * 255) | 0, 255],
};

// Sample the spectrum array at an absolute frequency.
//
// `centerFreq` and `twoSided` describe how the data array is laid out:
//   - one-sided (real signal): array spans 0 .. fs/2,         centerFreq ignored
//   - two-sided (complex/IQ):  array spans Fc-fs/2 .. Fc+fs/2 (fft-shifted)
export function safeSpecRead(data, freq, sr, centerFreq = 0, twoSided = false) {
  if (!data || !data.length || freq == null || !isFinite(freq) || sr <= 0)
    return -130;
  let t;
  if (twoSided) {
    t = ((freq - centerFreq) / sr) + 0.5;
  } else {
    t = freq / (sr / 2);
  }
  const idx = Math.min(
    data.length - 1,
    Math.max(0, Math.round(t * (data.length - 1)))
  );
  const v = data[idx];
  return isFinite(v) ? v : -130;
}
