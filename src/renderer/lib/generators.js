// Window functions
// Each entry: { fn(i, N) -> weight,  cg: coherent gain }
// cg is used to correct the FFT amplitude so levels stay consistent
// regardless of which window is chosen.

function besselI0(x) {
  let s = 1, term = 1;
  const xh = x * 0.5;
  for (let k = 1; k <= 20; k++) {
    term *= (xh / k) * (xh / k);
    s += term;
    if (term < 1e-10 * s) break;
  }
  return s;
}

const WIN = {
  rectangular: { fn: (_i, _N) => 1,                                                                     cg: 1.000 },
  hanning:     { fn: (i,  N)  => 0.5 * (1 - Math.cos((2 * Math.PI * i) / (N - 1))),                    cg: 0.500 },
  hamming:     { fn: (i,  N)  => 0.54 - 0.46 * Math.cos((2 * Math.PI * i) / (N - 1)),                  cg: 0.540 },
  blackman:    { fn: (i,  N)  => { const x = (2 * Math.PI * i) / (N - 1); return 0.42 - 0.5 * Math.cos(x) + 0.08 * Math.cos(2 * x); }, cg: 0.420 },
  flattop:     { fn: (i,  N)  => { const x = (2 * Math.PI * i) / (N - 1); return 1 - 1.93 * Math.cos(x) + 1.29 * Math.cos(2 * x) - 0.388 * Math.cos(3 * x) + 0.028 * Math.cos(4 * x); }, cg: 0.215 },
  kaiser:      { fn: (i,  N)  => { const b = 8, r = (2 * i) / (N - 1) - 1; return besselI0(b * Math.sqrt(Math.max(0, 1 - r * r))) / besselI0(b); }, cg: 0.400 },
};

// Cooley-Tukey iterative FFT (in-place, power-of-2 N)
function fft(re, im) {
  const N = re.length;
  // Bit-reversal permutation
  for (let i = 1, j = 0; i < N; i++) {
    let bit = N >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      let tmp = re[i]; re[i] = re[j]; re[j] = tmp;
      tmp = im[i]; im[i] = im[j]; im[j] = tmp;
    }
  }
  // Butterfly stages
  for (let len = 2; len <= N; len <<= 1) {
    const ang = -2 * Math.PI / len;
    const wRe = Math.cos(ang), wIm = Math.sin(ang);
    for (let i = 0; i < N; i += len) {
      let uRe = 1, uIm = 0;
      const half = len >> 1;
      for (let k = 0; k < half; k++) {
        const j = i + k + half;
        const tRe = uRe * re[j] - uIm * im[j];
        const tIm = uRe * im[j] + uIm * re[j];
        re[j] = re[i + k] - tRe;
        im[j] = im[i + k] - tIm;
        re[i + k] += tRe;
        im[i + k] += tIm;
        const nRe = uRe * wRe - uIm * wIm;
        uIm = uRe * wIm + uIm * wRe;
        uRe = nRe;
      }
    }
  }
}

// Multi-tone test signal
// [freq_normalized (0-1 of Nyquist), amplitude, time_rate]
// Amplitudes are chosen so peaks land at target dBFS after the
// coherent-gain correction: A -> 20*log10(A) dBFS.
//   0.0316 -> -30 dBFS,  0.0562 -> -25 dBFS,  0.0056 -> -45 dBFS
//   0.0018 -> -55 dBFS,  0.0010 -> -60 dBFS
const TONES = [
  [0.15, 0.0316, 0.50],  // fundamental      −30 dBFS
  [0.30, 0.0089, 0.50],  // 2nd harmonic     −41 dBFS
  [0.35, 0.0056, 0.30],  // secondary tone   −45 dBFS
  [0.45, 0.0045, 0.50],  // 3rd harmonic     −47 dBFS
  [0.52, 0.0018, 0.70],  // tertiary tone    −55 dBFS
  [0.72, 0.0562, 1.10],  // strong component −25 dBFS
  [0.88, 0.0010, 0.20],  // high-freq spur   −60 dBFS
];

function genTimeDomain(n, t) {
  const d = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    let v = (Math.random() - 0.5) * 0.00015; // ~−76 dBFS noise floor
    for (const [f, a, r] of TONES)
      v += a * (1 + 0.08 * Math.sin(r * t)) * Math.sin(Math.PI * f * i + t * r);
    d[i] = v;
  }
  return d;
}

// Public generators
export function genSpectrum(n, t, windowFn = "hanning", gain = 0) {
  const { fn: winFn, cg } = WIN[windowFn] || WIN.hanning;
  const signal = genTimeDomain(n, t);
  const re = new Float32Array(n);
  const im = new Float32Array(n);
  for (let i = 0; i < n; i++) re[i] = signal[i] * winFn(i, n);
  fft(re, im);
  const scale = 2 / (n * cg);
  const out = new Float32Array(n / 2);
  for (let i = 0; i < n / 2; i++) {
    const mag = Math.sqrt(re[i] * re[i] + im[i] * im[i]) * scale;
    out[i] = (mag > 1e-10 ? 20 * Math.log10(mag) : -130) + gain;
  }
  return out;
}

// Normalized [-1, 1] signal for the oscilloscope view
export function genWaveform(n, t) {
  const d = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    const x = (i / n) * Math.PI * 8;
    d[i] =
      0.6 * Math.sin(x + t) +
      0.25 * Math.sin(x * 3.01 + t * 1.7) +
      0.15 * Math.sin(x * 5.02 + t * 0.3) +
      (Math.random() - 0.5) * 0.08;
  }
  return d;
}

export function genIQ(n, t) {
  const re = new Float32Array(n),
    im = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    const a = (i / n) * Math.PI * 6 + t;
    re[i] = 0.7 * Math.cos(a) + 0.2 * Math.cos(a * 3.1) + (Math.random() - 0.5) * 0.15;
    im[i] = 0.7 * Math.sin(a) + 0.2 * Math.sin(a * 3.1) + (Math.random() - 0.5) * 0.15;
  }
  return { re, im };
}

export function genPhase(n, t) {
  const d = new Float32Array(n / 2);
  for (let i = 0; i < d.length; i++) {
    const f = i / d.length;
    d[i] =
      Math.atan2(Math.sin(f * 20 + t), Math.cos(f * 15 + t * 0.7)) +
      (Math.random() - 0.5) * 0.3;
  }
  return d;
}

export function genHistogram(wf) {
  const bins = 128,
    d = new Float32Array(bins);
  if (!wf) return d;
  for (let i = 0; i < wf.length; i++) {
    const bin = Math.min(bins - 1, Math.max(0, ((wf[i] + 1) * 0.5 * bins) | 0));
    d[bin]++;
  }
  const pk = Math.max(1, ...d);
  for (let i = 0; i < bins; i++) d[i] /= pk;
  return d;
}
