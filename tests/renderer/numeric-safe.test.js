// Pure-function tests for renderer math safety. None of these touch
// WebGL — they're the same helpers GPU renderers call before uploading
// uniforms or computing pixel coordinates, so a NaN here would propagate
// straight into the canvas.

import { describe, it, expect } from 'vitest';
import {
    valueToY,
    enforceMinSpread,
    safeFreqDenominator,
    clampLogPositive,
} from '../../src/renderer/lib/numeric-safe.js';

describe('valueToY', () => {
    it('maps the midpoint of [lo, hi] to the vertical midpoint of the plot', () => {
        // plot starts at y=10, height=100; lo=-100 dB, hi=-20 dB.
        // Value of -60 dB is midway; should map to y=10+50 = 60.
        expect(valueToY(-60, -100, -20, 10, 100)).toBeCloseTo(60, 5);
    });

    it('returns null when hi <= lo (no valid range)', () => {
        expect(valueToY(-50, -60, -60, 0, 100)).toBeNull();
        expect(valueToY(-50, -50, -60, 0, 100)).toBeNull();
    });

    it('returns null when value is not finite', () => {
        expect(valueToY(NaN, -100, -20, 0, 100)).toBeNull();
        expect(valueToY(Infinity, -100, -20, 0, 100)).toBeNull();
    });
});

describe('enforceMinSpread', () => {
    it('returns the input unchanged when spread >= minSpread', () => {
        const r = enforceMinSpread(-100, -20, 1);
        expect(r.lo).toBe(-100);
        expect(r.hi).toBe(-20);
    });

    it('recenters on the midpoint when hi - lo collapses below minSpread', () => {
        const r = enforceMinSpread(-60.0, -60.0, 1);
        expect(r.hi - r.lo).toBeCloseTo(1, 5);
        // Midpoint preserved.
        expect((r.hi + r.lo) / 2).toBeCloseTo(-60, 5);
    });

    it('handles a tiny but nonzero spread', () => {
        const r = enforceMinSpread(-60.1, -60.0, 1);
        expect(r.hi - r.lo).toBeCloseTo(1, 5);
        // Midpoint of -60.05 preserved.
        expect((r.hi + r.lo) / 2).toBeCloseTo(-60.05, 5);
    });
});

describe('safeFreqDenominator', () => {
    it('returns the sampleRate when positive and finite', () => {
        expect(safeFreqDenominator(48000)).toBe(48000);
    });

    it('returns null when sampleRate is 0', () => {
        expect(safeFreqDenominator(0)).toBeNull();
    });

    it('returns null when sampleRate is negative', () => {
        expect(safeFreqDenominator(-100)).toBeNull();
    });

    it('returns null when sampleRate is NaN or Infinity', () => {
        expect(safeFreqDenominator(NaN)).toBeNull();
        expect(safeFreqDenominator(Infinity)).toBeNull();
    });
});

describe('clampLogPositive', () => {
    it('returns the input when strictly positive and finite', () => {
        expect(clampLogPositive(1000)).toBe(1000);
        expect(clampLogPositive(1e-6)).toBe(1e-6);
    });

    it('returns the fallback when input is zero or negative', () => {
        expect(clampLogPositive(0, 1)).toBe(1);
        expect(clampLogPositive(-5, 1)).toBe(1);
    });

    it('returns the fallback when input is not finite', () => {
        expect(clampLogPositive(NaN, 1)).toBe(1);
        expect(clampLogPositive(Infinity, 1)).toBe(1);
    });

    it('defaults the fallback to 1 if not provided', () => {
        expect(clampLogPositive(0)).toBe(1);
    });
});
