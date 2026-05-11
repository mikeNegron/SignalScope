// Verifies the Hilbert transformer produces a true analytic signal
// (x + j·H[x], positive frequencies only) - not the conjugate-analytic
// (x − j·H[x], negative frequencies only). A signed test is critical here:
// both produce constant-magnitude output for sinusoidal input, so
// magnitude alone can't tell them apart.

#include <gtest/gtest.h>

#include "dsp/hilbert.hpp"

#include <cmath>
#include <complex>
#include <cstddef>
#include <numeric>
#include <vector>

using signalscope::HilbertTransformer;

namespace {

// Discrete radians per sample for a tone at f0 in a stream sampled at sr.
constexpr double omega(double f0, double sr) { return 2.0 * M_PI * f0 / sr; }

} // namespace

// Sign / phase: the load-bearing test
// For x = cos(ωi), the analytic signal is exp(jωi) = cos(ωi) + j·sin(ωi).
// We feed a long-enough block to clear the FIR's group-delay transient,
// then compare the imaginary output against the expected sine. If the
// loop iterates the delay line in the wrong direction, im[i] comes out
// as −sin(ω(i−GD)) and this assertion fires.
TEST(Hilbert, AnalyticOfCosineHasPositiveSineImaginary) {
    HilbertTransformer h;

    // Pick a frequency comfortably away from DC and Nyquist where the
    // 65-tap windowed-sinc Hilbert has near-unity gain.
    const double sr = 1024.0;
    const double f0 = 64.0;
    const double w  = omega(f0, sr);
    const std::size_t N = 4096;

    std::vector<float> x(N);
    for (std::size_t i = 0; i < N; i++) {
        x[i] = static_cast<float>(std::cos(w * i));
    }

    std::vector<float> re, im;
    h.process(x, re, im);

    // Skip past the group-delay transient. Use 4×GD as a comfortable
    // margin so the test isn't tight against the FIR ramp-up.
    const std::size_t GD   = HilbertTransformer::group_delay();
    const std::size_t skip = 4 * GD;

    // Re should track x[i−GD]. Im should track sin(ω·(i−GD)). Average
    // squared error compared to those references; tolerate a few percent
    // for FIR rolloff.
    double err_re = 0.0, err_im = 0.0, max_im_err = 0.0;
    std::size_t cnt = 0;
    for (std::size_t i = skip; i < N; i++) {
        const double t = static_cast<double>(i) - static_cast<double>(GD);
        const double expected_re = std::cos(w * t);
        const double expected_im = std::sin(w * t);
        const double dr = re[i] - expected_re;
        const double di = im[i] - expected_im;
        err_re += dr * dr;
        err_im += di * di;
        if (std::abs(di) > max_im_err) max_im_err = std::abs(di);
        cnt++;
    }
    err_re = std::sqrt(err_re / cnt);
    err_im = std::sqrt(err_im / cnt);

    EXPECT_LT(err_re, 0.01) << "Real component should be input delayed by GD";
    EXPECT_LT(err_im, 0.05)
        << "Imaginary component should be +sin(ω(i−GD)). RMS error too "
           "large - likely the convolution is iterating the delay line in "
           "the wrong direction (anti-symmetric Hilbert taps reverse -> "
           "negated kernel -> conjugate-analytic signal).";

    // Also fail loudly if the imaginary part has the WRONG SIGN. A
    // conjugate-analytic implementation would push max_im_err near 2.0
    // (sin and −sin differ by up to 2). Anything > 1.5 means the sign
    // is consistently flipped, not just FIR rolloff.
    EXPECT_LT(max_im_err, 0.5)
        << "Peak |im−sin| > 0.5 - Hilbert sign is flipped (producing "
           "x − j·H[x], the conjugate-analytic signal with only negative "
           "frequencies).";
}

// Magnitude: sanity
// |x + j·H[x]| should be ≈ amplitude of the input cosine. Catches
// catastrophic gain errors.
TEST(Hilbert, AnalyticMagnitudeIsConstantForSinusoid) {
    HilbertTransformer h;
    const double sr = 1024.0;
    const double f0 = 100.0;
    const double w  = omega(f0, sr);
    const std::size_t N = 2048;
    const float A = 0.7f;

    std::vector<float> x(N);
    for (std::size_t i = 0; i < N; i++) x[i] = A * std::cos(w * i);

    std::vector<float> re, im;
    h.process(x, re, im);

    // Average magnitude over steady state. Allow a few percent of slack
    // for the windowed-sinc rolloff.
    double sum_mag = 0.0;
    std::size_t cnt = 0;
    const std::size_t skip = 4 * HilbertTransformer::group_delay();
    for (std::size_t i = skip; i < N; i++) {
        sum_mag += std::sqrt(re[i] * re[i] + im[i] * im[i]);
        cnt++;
    }
    const double avg = sum_mag / cnt;
    EXPECT_NEAR(avg, A, 0.05);
}

// Frequency-domain sign test
// DFT of (re + j·im) for an analytic signal of cos(ωi) should peak at
// +ω, not −ω. We probe both bins via Goertzel-style inner products and
// compare. If the conjugate-analytic bug is present, the negative-bin
// magnitude wins.
TEST(Hilbert, PositiveFrequencyBinDominatesNegative) {
    HilbertTransformer h;
    const double sr = 1024.0;
    const double f0 = 100.0;
    const double w  = omega(f0, sr);
    const std::size_t N = 2048;

    std::vector<float> x(N);
    for (std::size_t i = 0; i < N; i++) x[i] = std::cos(w * i);

    std::vector<float> re, im;
    h.process(x, re, im);

    // Inner-product the analytic signal with exp(±j·ω·i) over steady
    // state. The bin matching the signal's true sign-of-frequency wins
    // by orders of magnitude; the anti-bin should be near zero.
    std::complex<double> pos{0, 0}, neg{0, 0};
    const std::size_t skip = 4 * HilbertTransformer::group_delay();
    for (std::size_t i = skip; i < N; i++) {
        const std::complex<double> z(re[i], im[i]);
        const std::complex<double> rot_pos = std::polar(1.0, -w * i); // e^{-jω i}
        const std::complex<double> rot_neg = std::polar(1.0,  w * i); // e^{+jω i}
        pos += z * rot_pos;
        neg += z * rot_neg;
    }
    const double mag_pos = std::abs(pos);
    const double mag_neg = std::abs(neg);

    EXPECT_GT(mag_pos, mag_neg * 10.0)
        << "Positive-frequency bin should dominate. mag_pos=" << mag_pos
        << " mag_neg=" << mag_neg
        << " - if neg dominates, the analytic signal has the wrong sign.";
}
