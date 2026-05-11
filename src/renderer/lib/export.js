// PNG screenshot of the largest canvas on the page. Renderer-side
// because the pixels only exist in the browser context. WAV / AIFF /
// CSV / RAW exports live on the backend (export_capture command).

function dl(blob, name) {
  const u = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = u;
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(u), 1e3);
}

export function exportPNG() {
  const cs = document.querySelectorAll("canvas");
  if (!cs.length) return;
  let best = cs[0];
  for (const c of cs)
    if (c.width * c.height > best.width * best.height) best = c;
  best.toBlob((b) => {
    if (b) dl(b, `signalscope_${Date.now()}.png`);
  }, "image/png");
}
