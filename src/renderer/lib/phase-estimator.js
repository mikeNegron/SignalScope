// Sub-FFT-frame scroll interpolator for the spectrogram.
//
// Background: the spectrogram texture's head pointer advances exactly
// once per FFT arrival. On a 60Hz display with a slower FFT rate (e.g.
// 4-12Hz at default FFT sizes), the GPU draws the same texture state
// for many display frames in a row, producing visible stepping. This
// helper estimates the inter-arrival period (EMA over recent intervals)
// and reports the wall-clock-driven fractional phase in [0, 1] used as
// a sub-row offset in the shader. Combined with gl.LINEAR filtering on
// the data texture, this produces continuous motion at display rate.

const PERIOD_ALPHA = 0.2;

export function makePhaseEstimator() {
    let lastArrival = null;
    let periodEma   = null;
    return {
        onArrival(nowMs) {
            if (lastArrival != null) {
                const dt = nowMs - lastArrival;
                if (dt > 0) {
                    periodEma = periodEma == null
                        ? dt
                        : PERIOD_ALPHA * dt + (1 - PERIOD_ALPHA) * periodEma;
                }
            }
            lastArrival = nowMs;
        },
        getPhase(nowMs) {
            if (lastArrival == null || periodEma == null || periodEma <= 0) return 0;
            const elapsed = nowMs - lastArrival;
            if (elapsed <= 0) return 0;
            const phase = elapsed / periodEma;
            return phase > 1 ? 1 : phase;
        },
        reset() {
            lastArrival = null;
            periodEma   = null;
        },
    };
}
