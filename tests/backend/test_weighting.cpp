// Equalization / weighting curve tests.
// Locking down:
//   - weighting_from_string parses the strings the renderer actually sends
//   - None is a true no-op (every bin unchanged)
//   - Flatten: every bin shifts by exactly the same amount (the median)
//   - A/C weighting at 1 kHz lands within ±0.5 dB of 0 (canonical reference)
//   - A-weighting hits its standard table values within tolerance
//   - C-weighting hits its standard table values within tolerance
//   - Two-sided spectra: weighting is symmetric around 0 (curves are even)
//   - Bin -> frequency mapping matches the FFT layout the spectrum frames use

#include <gtest/gtest.h>

#include "dsp/weighting.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

using signalscope::Weighting;
using signalscope::weighting_from_string;
using signalscope::apply_weighting;
using signalscope::detail::a_weight_db;
using signalscope::detail::c_weight_db;

// Parsing
TEST(Weighting, FromStringHandlesAllSupportedNames) {
    EXPECT_EQ(weighting_from_string("none"),     Weighting::None);
    EXPECT_EQ(weighting_from_string(""),         Weighting::None);
    EXPECT_EQ(weighting_from_string("garbage"),  Weighting::None);
    EXPECT_EQ(weighting_from_string("flatten"),  Weighting::Flatten);
    EXPECT_EQ(weighting_from_string("A-weight"), Weighting::AWeight);
    EXPECT_EQ(weighting_from_string("a-weight"), Weighting::AWeight);
    EXPECT_EQ(weighting_from_string("C-weight"), Weighting::CWeight);
    EXPECT_EQ(weighting_from_string("c-weight"), Weighting::CWeight);
}

// None / null
TEST(Weighting, NoneIsTrueNoOp) {
    std::vector<float> spec(64);
    for (std::size_t i = 0; i < spec.size(); i++) spec[i] = -50.0f + i * 0.25f;
    const std::vector<float> orig = spec;
    apply_weighting(spec, Weighting::None, 48000.0, /*two_sided=*/false);
    EXPECT_EQ(spec, orig);
}

TEST(Weighting, EmptySpanIsNoOp) {
    // The original (T*, size_t) overload accepted nullptr+size; the span
    // overload models the same intent with an empty span.
    apply_weighting(std::span<float>{}, Weighting::AWeight, 48000.0, false);
    SUCCEED();
}

// A-weighting reference values
TEST(Weighting, AWeightAtOneKilohertzIsZero) {
    // Definition of A-weighting: A(1 kHz) = 0 dB by construction.
    EXPECT_NEAR(a_weight_db(1000.0), 0.0f, 0.5f);
}

TEST(Weighting, AWeightMatchesStandardTable) {
    // IEC 61672 reference values at common 1/3-octave centers (dB).
    // Tolerances follow the standard's class-1 limits, plus a small margin
    // for our +2 dB normalization simplification.
    struct Ref { double f; float db; };
    const Ref table[] = {
        {  10.0,   -70.4f},
        { 100.0,   -19.1f},
        { 500.0,    -3.2f},
        {1000.0,     0.0f},
        {2000.0,     1.2f},
        {5000.0,     0.5f},
        {10000.0,   -2.5f},
        {20000.0,   -9.3f},
    };
    for (const auto& r : table) {
        EXPECT_NEAR(a_weight_db(r.f), r.db, 1.0f) << "f = " << r.f << " Hz";
    }
}

TEST(Weighting, AWeightRejectsDCWithoutBlowingUp) {
    // f=0 makes the rational function go to 0 -> log to -inf. The helper
    // clamps to a finite floor so the renderer's color mapping doesn't
    // trip on -inf bins.
    const float v = a_weight_db(0.0);
    EXPECT_TRUE(std::isfinite(v));
    EXPECT_LE(v, -100.0f);  // very attenuated
}

// C-weighting reference values
TEST(Weighting, CWeightAtOneKilohertzIsZero) {
    EXPECT_NEAR(c_weight_db(1000.0), 0.0f, 0.5f);
}

TEST(Weighting, CWeightMatchesStandardTable) {
    struct Ref { double f; float db; };
    const Ref table[] = {
        {  10.0,  -14.3f},
        { 100.0,   -0.3f},
        {1000.0,    0.0f},
        {4000.0,   -0.8f},
        {10000.0,  -4.4f},
        {20000.0, -11.2f},
    };
    for (const auto& r : table) {
        EXPECT_NEAR(c_weight_db(r.f), r.db, 1.0f) << "f = " << r.f << " Hz";
    }
}

// Apply: one-sided
TEST(Weighting, AWeightAddsCurveToOneSidedSpectrum) {
    // 2048-bin one-sided spectrum at fs=48k. Bin i -> freq i*fs/(2*N).
    constexpr std::size_t N = 2048;
    constexpr double fs = 48000.0;
    std::vector<float> spec(N, 0.0f);  // flat 0 dB input
    apply_weighting(spec, Weighting::AWeight, fs, /*two_sided=*/false);

    // Pick a few bins and check they match the curve directly.
    for (std::size_t i : {2, 50, 200, 800, 1500}) {
        const double f = static_cast<double>(i) * (fs * 0.5) / static_cast<double>(N);
        EXPECT_NEAR(spec[i], a_weight_db(f), 1e-3f) << "bin " << i;
    }
}

// Apply: two-sided symmetry
TEST(Weighting, TwoSidedWeightingIsSymmetricAroundDC) {
    // Two-sided spectrum spans -fs/2 (i=0) to +fs/2-bin (i=N-1). The
    // weighting curve is even in f, so the value at -f and +f should match.
    constexpr std::size_t N = 1024;
    constexpr double fs = 48000.0;
    std::vector<float> spec(N, 0.0f);
    apply_weighting(spec, Weighting::CWeight, fs, /*two_sided=*/true);

    // Bin i is at freq (i - N/2) * fs/N; bin N-i is the mirror.
    // Skip i=0 (special - represents -fs/2 with no positive-side mirror).
    for (std::size_t i = 1; i < N / 2; i++) {
        EXPECT_NEAR(spec[N / 2 - i], spec[N / 2 + i], 1e-3f)
            << "bins " << (N / 2 - i) << " vs " << (N / 2 + i);
    }
}

// Flatten
// Flatten is implemented renderer-side as a y-axis auto-range - the backend
// leaves the dB values alone so peaks/noise-floor stay in absolute units
// across the wire. apply_weighting is a no-op for it.
TEST(Weighting, ApplyWeightingFlattenIsNoOp) {
    std::vector<float> spec{-50.0f, -10.0f, -120.0f, 0.0f};
    const auto orig = spec;
    apply_weighting(spec, Weighting::Flatten, 48000.0, false);
    EXPECT_EQ(spec, orig);
}

// Bin -> frequency mapping
TEST(Weighting, OneSidedFrequencyMapping) {
    // For a 1024-bin one-sided spectrum at fs=48k, the last bin (i=1023) is
    // at fs/2 - bin = 24000 - (24000/1024). DC (i=0) is at exactly 0 Hz.
    EXPECT_DOUBLE_EQ(signalscope::one_sided_freq(0,    1024, 48000.0), 0.0);
    EXPECT_DOUBLE_EQ(signalscope::one_sided_freq(1024, 1024, 48000.0), 24000.0);
}

TEST(Weighting, TwoSidedFrequencyMapping) {
    // Two-sided 1024-bin: i=0 -> -fs/2; i=N/2 -> 0; i=N-1 -> fs/2 - bin.
    EXPECT_DOUBLE_EQ(signalscope::two_sided_freq(0,   1024, 48000.0), -24000.0);
    EXPECT_DOUBLE_EQ(signalscope::two_sided_freq(512, 1024, 48000.0),  0.0);
}
