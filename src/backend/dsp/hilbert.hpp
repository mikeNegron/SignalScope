#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>
#include <vector>

namespace signalscope {

// Hilbert transformer - converts a real input to its analytic representation
// (a complex signal with zero negative-frequency content). Used as an
// optional pre-stage in front of the DDC so that tuning a real source does
// not produce the conjugate-symmetric mirror images you'd otherwise see in
// a two-sided spectrum.
//
// Implementation: 257-tap windowed-sinc Hilbert FIR (Blackman window). The
// ideal Hilbert impulse response is
//     h_ideal[n] = 2 / (π·n)  for n odd, 0 for n even
// (centered at n=0). After centering on the FIR midpoint and applying a
// window we get a finite-length anti-symmetric kernel. The FIR delays its
// input by (N-1)/2 samples, so the real component of the analytic signal
// must be drawn from the same delayed sample to keep the (re, im) pair
// time-aligned. We do that by reading the center tap of the same delay
// line the FIR is convolving against - no separate buffer.
//
// Tap count + window choice tradeoffs:
//   - Stopband attenuation determines how cleanly negative-frequency
//     mirrors are cancelled. Blackman gives ~−74 dB (vs ~−53 dB for
//     Hamming) so a residual mirror at, say, 440 Hz on a 48 kHz source
//     ends up well below the noise floor instead of visible in the
//     spectrogram.
//   - Useful band edges sit ~1.5/N of the sample rate from DC and Nyquist.
//     At N=257 that's about 0.6% of fs - for 48 kHz, mirrors of tones
//     above ~300 Hz are suppressed cleanly. Below that the FIR's gain at
//     ±f is no longer well-separated and some mirror leakage remains.
//   - Group delay is (N−1)/2 = 128 samples = 2.7 ms at 48 kHz.
//   - Going larger (e.g. 1025) extends the useful band down toward ~75 Hz
//     at the cost of latency and ~4× CPU per sample. Diminishing returns
//     for visualization use.
class HilbertTransformer {
public:
    HilbertTransformer() {
        rebuild_taps();
        history_.assign(N, 0.0f);
    }

    void reset() {
        std::fill(history_.begin(), history_.end(), 0.0f);
        idx_ = 0;
    }

    // Group delay in samples - same value used by callers that need to
    // align the FIR's output with non-Hilbert paths. Exposed so tests
    // and any future analytic-path consumers don't have to hard-code it.
    static constexpr std::size_t group_delay() { return GD; }

    // Convert a block of REAL samples into an analytic signal. out_re and
    // out_im are sized to `n` and aligned in time (no separate group-delay
    // compensation needed by callers).
    //
    // Convolution order matters here: the standard FIR convention is
    //     y[n] = Σ h[k]·x[n-k]   (taps[0] applies to the NEWEST sample)
    // We iterate the delay line newest -> oldest so taps_[0] hits the most
    // recent sample. Iterating the other direction would apply taps_
    // reversed; for an *anti-symmetric* kernel like a Hilbert that's
    // equivalent to negating it, which produces the conjugate-analytic
    // signal (positive frequencies cancelled, negatives doubled). The
    // unit test in test_hilbert.cpp guards against that regression.
    void process(std::span<const float> in,
                 std::vector<float>& out_re,
                 std::vector<float>& out_im)
    {
        const std::size_t n = in.size();
        out_re.resize(n);
        out_im.resize(n);
        for (std::size_t i = 0; i < n; i++) {
            history_[idx_] = in[i];
            idx_ = (idx_ + 1) % N;

            // Walk newest -> oldest. After the increment above, idx_ points
            // at the OLDEST slot (where the next sample will overwrite),
            // so the most recent write lives at (idx_-1) mod N. Wrapping
            // arithmetic with N additions/modulo avoids signed off-by-ones.
            float acc = 0.0f;
            float real_at_center = 0.0f;
            std::size_t h = (idx_ + N - 1) % N;
            for (std::size_t k = 0; k < N; k++) {
                const float s = history_[h];
                acc += taps_[k] * s;
                if (k == GD) real_at_center = s;
                h = (h + N - 1) % N;
            }
            out_re[i] = real_at_center;
            out_im[i] = acc;
        }
    }

private:
    static constexpr std::size_t N  = 257;
    static constexpr std::size_t GD = (N - 1) / 2; // 128-sample group delay

    std::vector<float> taps_;
    std::vector<float> history_;
    std::size_t idx_ = 0;

    void rebuild_taps() {
        taps_.resize(N);
        const int gd = static_cast<int>(GD);
        constexpr double pi = std::numbers::pi;
        for (int n = 0; n < static_cast<int>(N); n++) {
            const int k = n - gd;
            double tap;
            if (k == 0)              tap = 0.0;
            else if ((k & 1) == 0)   tap = 0.0;       // even-offset taps are 0
            else                     tap = 2.0 / (pi * static_cast<double>(k));
            // Blackman window - ~−74 dB stopband, materially better mirror
            // suppression than Hamming for the same tap count. The wider
            // main lobe is harmless for the Hilbert use case since we're
            // not trying to resolve closely-spaced features.
            const double phase = 2.0 * pi *
                static_cast<double>(n) / static_cast<double>(N - 1);
            const double w = 0.42 - 0.5 * std::cos(phase)
                                  + 0.08 * std::cos(2.0 * phase);
            taps_[n] = static_cast<float>(tap * w);
        }
    }
};

} // namespace signalscope
