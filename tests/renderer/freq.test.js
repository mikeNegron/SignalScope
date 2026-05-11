// Pure-function tests for src/renderer/lib/freq.js - the freq math used
// by the spectrum/spectrogram axes, cursors, peaks, and click handler.
// Bugs here cascade into every visible Hz label, so the round-trip
// invariants (tToFreq ∘ freqToT == identity) deserve explicit coverage.

import { describe, expect, it } from 'vitest';
import {
  LOG_FMIN_HZ,
  binToFreq,
  fmtFreqLong,
  fmtFreqShort,
  freqToT,
  logK,
  tToBin,
  tToFreq,
} from '../../src/renderer/lib/freq.js';

const SR = 48000;

// Approximate equality for float math. tToFreq/freqToT compose with
// log/exp on the log path, which loses a few ULP - 1e-6 relative tol
// is plenty.
function approxEq(a, b, tol = 1e-6) {
  return Math.abs(a - b) <= tol * Math.max(1, Math.abs(a), Math.abs(b));
}

// Linear axis

describe('tToFreq / freqToT - linear, one-sided', () => {
  it('maps t=0 to 0 Hz', () => {
    expect(tToFreq(0, SR, 0, false)).toBe(0);
  });
  it('maps t=1 to fs/2', () => {
    expect(tToFreq(1, SR, 0, false)).toBe(SR / 2);
  });
  it('maps t=0.5 to fs/4', () => {
    expect(tToFreq(0.5, SR, 0, false)).toBe(SR / 4);
  });
  it('round-trips f -> t -> f for a sweep', () => {
    for (let i = 0; i < 100; i++) {
      const f = (i / 99) * (SR / 2);
      const t = freqToT(f, SR, 0, false);
      const f2 = tToFreq(t, SR, 0, false);
      expect(approxEq(f, f2)).toBe(true);
    }
  });
});

describe('tToFreq / freqToT - linear, two-sided (IQ)', () => {
  it('maps t=0.5 to centerFreq', () => {
    const cf = 1.0e6;
    expect(tToFreq(0.5, SR, cf, true)).toBe(cf);
  });
  it('maps t=0 to cf − fs/2 and t=1 to cf + fs/2', () => {
    const cf = 1.0e6;
    expect(tToFreq(0, SR, cf, true)).toBe(cf - SR / 2);
    expect(tToFreq(1, SR, cf, true)).toBe(cf + SR / 2);
  });
  it('round-trips around an arbitrary center freq', () => {
    const cf = 2.45e9; // 2.4 GHz band
    for (let i = 0; i < 50; i++) {
      const t = i / 49;
      const f = tToFreq(t, SR, cf, true);
      const t2 = freqToT(f, SR, cf, true);
      expect(approxEq(t, t2)).toBe(true);
    }
  });
  it('ignores log scale request when twoSided is set', () => {
    // Negative frequencies have no log; the helpers must transparently
    // fall back to linear so the renderer doesn't have to special-case.
    const cf = 1.0e6;
    expect(tToFreq(0.5, SR, cf, true, 'log')).toBe(cf);
  });
});

// Log axis

describe('tToFreq / freqToT - logarithmic, one-sided', () => {
  it('maps t=0 to LOG_FMIN_HZ', () => {
    expect(tToFreq(0, SR, 0, false, 'log')).toBe(LOG_FMIN_HZ);
  });
  it('maps t=1 to fs/2', () => {
    expect(tToFreq(1, SR, 0, false, 'log')).toBe(SR / 2);
  });
  it('maps t=0.5 to the geometric mean of [fmin, fmax]', () => {
    const fmin = LOG_FMIN_HZ;
    const fmax = SR / 2;
    const expected = fmin * Math.sqrt(fmax / fmin); // = sqrt(fmin·fmax)
    const got = tToFreq(0.5, SR, 0, false, 'log');
    expect(approxEq(got, expected)).toBe(true);
  });
  it('round-trips over the full axis', () => {
    for (let i = 0; i < 100; i++) {
      const t = i / 99;
      const f = tToFreq(t, SR, 0, false, 'log');
      const t2 = freqToT(f, SR, 0, false, 'log');
      expect(approxEq(t, t2, 1e-5)).toBe(true);
    }
  });
  it('clamps freqToT to 0 for f below fmin', () => {
    expect(freqToT(LOG_FMIN_HZ / 2, SR, 0, false, 'log')).toBe(0);
    expect(freqToT(0, SR, 0, false, 'log')).toBe(0);
  });
});

// logK helper (shared with shaders)

describe('logK', () => {
  it('returns ln(fmax/fmin) consistently with the math freqToT uses', () => {
    const k = logK(SR);
    expect(approxEq(k, Math.log(SR / 2 / LOG_FMIN_HZ))).toBe(true);
  });
  it('grows with sample rate', () => {
    expect(logK(96000)).toBeGreaterThan(logK(48000));
    expect(logK(192000)).toBeGreaterThan(logK(96000));
  });
});

// tToBin / binToFreq

describe('tToBin', () => {
  it('clamps to [0, dataLength-1]', () => {
    expect(tToBin(-1,  100)).toBe(0);
    expect(tToBin( 0,  100)).toBe(0);
    expect(tToBin( 1,  100)).toBe(99);
    expect(tToBin( 2,  100)).toBe(99);
  });
  it('rounds to nearest', () => {
    expect(tToBin(0.5, 101)).toBe(50);
  });
  it('handles dataLength=1 without crashing', () => {
    expect(tToBin(0.5, 1)).toBe(0);
  });
  it('handles dataLength=0 by returning 0 (defensive)', () => {
    // Internal Math.max(1, ...) means dataLength<1 still returns a
    // valid index. Guards renderer paths during reconnection where
    // arrays are briefly empty.
    expect(tToBin(0.5, 0)).toBe(0);
  });
});

describe('binToFreq', () => {
  it('uses bin-center mapping (offset by 0.5)', () => {
    // bin 0 of an N=100 spectrum corresponds to t = 0.5/100 = 0.005,
    // i.e. fs/2 * 0.005 = 120 Hz at fs=48000. Verifies the sub-bin
    // offset that the renderer's grid + cursor math depends on.
    expect(binToFreq(0, 100, SR, 0, false)).toBe((SR / 2) * (0.5 / 100));
  });
  it('agrees with tToFreq when given the same t', () => {
    const t = (42 + 0.5) / 100;
    expect(binToFreq(42, 100, SR, 0, false)).toBe(tToFreq(t, SR, 0, false));
  });
});

// Formatters

describe('fmtFreqShort', () => {
  it('uses unit suffix at decade boundaries', () => {
    expect(fmtFreqShort(440)).toBe('440');
    expect(fmtFreqShort(1500)).toBe('1.5k');
    expect(fmtFreqShort(2.4e6)).toBe('2.40M');
    expect(fmtFreqShort(2.4e9)).toBe('2.400G');
  });
  it('preserves sign for negative frequencies (two-sided plots)', () => {
    expect(fmtFreqShort(-1500)).toBe('-1.5k');
  });
  it('handles non-finite input gracefully', () => {
    expect(fmtFreqShort(NaN)).toBe('-');
    expect(fmtFreqShort(Infinity)).toBe('-');
    expect(fmtFreqShort(-Infinity)).toBe('-');
  });
});

describe('fmtFreqLong', () => {
  it('appends Hz/kHz/MHz/GHz with full precision', () => {
    expect(fmtFreqLong(440)).toBe('440.0 Hz');
    expect(fmtFreqLong(2400)).toBe('2.40 kHz');
    expect(fmtFreqLong(2.4001e6)).toBe('2.4001 MHz');
  });
  it('handles non-finite input gracefully', () => {
    expect(fmtFreqLong(NaN)).toBe('-');
  });
});
