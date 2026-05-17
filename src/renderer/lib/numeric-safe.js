// Small pure helpers for renderer math that need to stay NaN/Infinity
// safe. Extracted from the GPU renderers and the flatten-mode auto-range
// loop so they can be unit-tested without a WebGL mock — and so multiple
// renderers can share the same correctness contract.

// Map a value in [lo, hi] to a pixel Y in [top, top + height]. Returns
// null when the range is empty (hi <= lo) or the value is non-finite —
// callers should skip rendering rather than clamp a NaN.
export function valueToY(value, lo, hi, top, height) {
    if (!Number.isFinite(value)) return null;
    const span = hi - lo;
    if (!(span > 0)) return null;
    return top + ((hi - value) / span) * height;
}

// Force a minimum spread between hi and lo. When the input collapses
// (e.g., flat spectrum drives the flatten EMA to a single value), the
// downstream divisions go to Infinity / NaN. Recenter on the midpoint
// and inflate to the minimum spread so all consumers see a usable range.
export function enforceMinSpread(lo, hi, minSpread) {
    if (hi - lo >= minSpread) return { lo, hi };
    const mid = (lo + hi) / 2;
    const half = minSpread / 2;
    return { lo: mid - half, hi: mid + half };
}

// Validate a sample rate before using it as a divisor or logarithm
// argument. Returns the rate when positive and finite; null otherwise.
// Renderer code that gets null should bail out — pre-connect frequency
// labels read "--" instead of "0 Hz" or "Infinity Hz".
export function safeFreqDenominator(sampleRate) {
    if (!Number.isFinite(sampleRate) || sampleRate <= 0) return null;
    return sampleRate;
}

// Clamp an argument to a logarithm to a strictly-positive fallback when
// it would otherwise produce NaN or -Infinity. Used by grid-tick spacing
// computations that should never crash on a temporarily-misconfigured
// dB range or fmin.
export function clampLogPositive(value, fallback = 1) {
    if (!Number.isFinite(value) || value <= 0) return fallback;
    return value;
}
