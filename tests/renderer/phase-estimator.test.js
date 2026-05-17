// Tests the PhaseEstimator helper that drives the spectrogram's
// sub-FFT-frame scroll interpolation. The helper is fed wall-clock
// timestamps as new spectrum frames arrive and emits a phase in
// [0, 1] representing "how far through the expected next-frame
// interval are we?" — used as a sub-row offset in the shader.

import { describe, it, expect } from 'vitest';
import { makePhaseEstimator } from '../../src/renderer/lib/phase-estimator.js';

describe('makePhaseEstimator', () => {
    it('returns 0 before any arrivals (cannot estimate period)', () => {
        const e = makePhaseEstimator();
        expect(e.getPhase(100)).toBe(0);
    });

    it('returns 0 after a single arrival (period still unknown)', () => {
        const e = makePhaseEstimator();
        e.onArrival(100);
        expect(e.getPhase(110)).toBe(0);
        expect(e.getPhase(150)).toBe(0);
    });

    it('reports phase linearly between two arrivals once period is known', () => {
        const e = makePhaseEstimator();
        e.onArrival(0);
        e.onArrival(100);
        expect(e.getPhase(125)).toBeCloseTo(0.25, 5);
        expect(e.getPhase(150)).toBeCloseTo(0.5,  5);
        expect(e.getPhase(199)).toBeCloseTo(0.99, 5);
    });

    it('clamps phase at 1.0 if the next arrival is overdue', () => {
        const e = makePhaseEstimator();
        e.onArrival(0);
        e.onArrival(100);
        expect(e.getPhase(200)).toBe(1);
        expect(e.getPhase(500)).toBe(1);
    });

    it('never returns a negative phase if the clock goes backwards', () => {
        const e = makePhaseEstimator();
        e.onArrival(0);
        e.onArrival(100);
        expect(e.getPhase(99)).toBe(0);
        expect(e.getPhase(-50)).toBe(0);
    });

    it('smooths irregular periods via EMA (period stays near recent average)', () => {
        const e = makePhaseEstimator();
        e.onArrival(0);
        e.onArrival(100);
        e.onArrival(200);
        e.onArrival(400);

        const phase = e.getPhase(500);
        expect(phase).toBeGreaterThan(0.5);
        expect(phase).toBeLessThan(1.0);
    });

    it('reset clears state so a new source starts fresh', () => {
        const e = makePhaseEstimator();
        e.onArrival(0);
        e.onArrival(100);
        expect(e.getPhase(150)).toBeCloseTo(0.5, 5);
        e.reset();
        expect(e.getPhase(150)).toBe(0);
        e.onArrival(200);
        expect(e.getPhase(250)).toBe(0);
    });
});
