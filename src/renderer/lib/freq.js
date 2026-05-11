// Frequency-axis math + formatting.
// The spectrum panel can display two different x-axes:
//   - One-sided (real source):       0 .. fs/2,             Fc is ignored
//   - Two-sided (complex/IQ):  Fc - fs/2 .. Fc + fs/2 (fft-shifted)
// All cursor frequencies, peak labels, and grid ticks go through these
// helpers so the user always sees absolute Hz - no mental math.
// Scale: either 'linear' or 'log'. Log scale is one-sided only - for IQ
// (two-sided) sources the helpers transparently fall back to linear because
// log of negative frequency is undefined. Log axis spans [LOG_FMIN_HZ, fs/2]
// with 20 Hz as the lower bound (audio-grade default).

export const LOG_FMIN_HZ = 20;

// True when log axis is meaningful for this configuration.
function logActive(twoSided, scale) {
  return scale === 'log' && !twoSided;
}

/// Map a fractional x position (0..1, left -> right of plot) to an absolute
/// frequency in Hz.
export function tToFreq(t, sampleRate, centerFreq, twoSided, scale = 'linear') {
  if (logActive(twoSided, scale)) {
    const fmax = sampleRate / 2;
    const fmin = LOG_FMIN_HZ;
    return fmin * Math.pow(fmax / fmin, t);
  }
  if (twoSided) return centerFreq + (t - 0.5) * sampleRate;
  return t * (sampleRate / 2);
}

/// Inverse of tToFreq: map an absolute frequency in Hz back to a 0..1 x
/// position in the plot. Result is unclamped - callers can clamp if needed.
export function freqToT(f, sampleRate, centerFreq, twoSided, scale = 'linear') {
  if (logActive(twoSided, scale)) {
    const fmax = sampleRate / 2;
    const fmin = LOG_FMIN_HZ;
    if (f <= fmin) return 0;
    return Math.log(f / fmin) / Math.log(fmax / fmin);
  }
  if (twoSided) return ((f - centerFreq) / sampleRate) + 0.5;
  return f / (sampleRate / 2);
}

/// Map a fractional x position to a spectrum-data bin index, given a payload
/// length (one-sided length = fft_size/2; two-sided length = fft_size).
/// Returns a clamped integer index.
export function tToBin(t, dataLength) {
  const n = Math.max(1, dataLength | 0);
  const idx = Math.round(t * (n - 1));
  return Math.max(0, Math.min(n - 1, idx));
}

/// Map a bin index to absolute frequency.
export function binToFreq(bin, dataLength, sampleRate, centerFreq, twoSided) {
  const t = (bin + 0.5) / dataLength;
  return tToFreq(t, sampleRate, centerFreq, twoSided);
}

/// Natural-log of fmax/fmin for the log axis. Shared by JS callers and
/// uploaded as a shader uniform - keeps the GPU and CPU sides in sync on
/// where each frequency lands.
export function logK(sampleRate) {
  return Math.log(sampleRate / 2 / LOG_FMIN_HZ);
}

// Formatting
/// Compact axis-tick formatting: "2.4G", "100M", "12.5k", "440".
/// `decimals` overrides the unit-specific defaults.
export function fmtFreqShort(hz, decimals) {
  if (!isFinite(hz)) return "-";
  const a = Math.abs(hz);
  if (a >= 1e9) return (hz / 1e9).toFixed(decimals ?? 3) + "G";
  if (a >= 1e6) return (hz / 1e6).toFixed(decimals ?? 2) + "M";
  if (a >= 1e3) return (hz / 1e3).toFixed(decimals ?? 1) + "k";
  return hz.toFixed(decimals ?? 0);
}

/// Verbose readout: "2.401 GHz", "12.45 kHz", "440 Hz". Used by cursor + peak
/// labels where the user wants an exact value.
export function fmtFreqLong(hz, decimals) {
  if (!isFinite(hz)) return "-";
  const a = Math.abs(hz);
  if (a >= 1e9) return (hz / 1e9).toFixed(decimals ?? 6) + " GHz";
  if (a >= 1e6) return (hz / 1e6).toFixed(decimals ?? 4) + " MHz";
  if (a >= 1e3) return (hz / 1e3).toFixed(decimals ?? 2) + " kHz";
  return hz.toFixed(decimals ?? 1) + " Hz";
}
