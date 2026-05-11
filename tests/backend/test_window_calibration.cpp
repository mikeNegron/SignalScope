// Window-calibration tests.
// What we're locking down:
//   - coherent_gain() and enbw_bins() match published values for the
//     standard windows we ship (Heinzel, Rüdiger & Schilling 2002).
//   - A unit-amplitude tone perfectly aligned with a bin reads back at
//     the same dB level regardless of window choice - proves the
//     coherent-gain compensation actually works in the FFT path.

#include <gtest/gtest.h>

#include "dsp/dsp_pipeline.hpp"

#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

using signalscope::DSPPipeline;

namespace {

// Build a unit-amplitude cosine of length N at a bin-aligned frequency
// (so the FFT energy lands in exactly one bin, no leakage).
std::vector<float> bin_aligned_cosine(std::size_t N, std::size_t bin) {
    std::vector<float> x(N);
    const double w = 2.0 * std::numbers::pi_v<double> * static_cast<double>(bin) /
                     static_cast<double>(N);
    for (std::size_t i = 0; i < N; i++) x[i] = std::cos(w * i);
    return x;
}

float peak_db(const std::vector<float>& spec) {
    float m = -1e9f;
    for (float v : spec) if (v > m) m = v;
    return m;
}

} // namespace

// Per-window calibration constants
TEST(WindowCalibration, ConstantsMatchPublishedValues) {
    // Reference values from Heinzel et al., "Spectrum and spectral
    // density estimation by the Discrete Fourier transform (DFT),
    // including a comprehensive list of window functions and some new
    // flat-top windows" (2002). N=4096, large enough that finite-N
    // corrections are well below 1%.
    constexpr std::size_t N = 4096;
    DSPPipeline dsp({.fft_size = N});

    struct Ref { const char* name; float cg; float enbw; };
    const Ref refs[] = {
        // name        coherent_gain   ENBW (bins)
        {"rectangular", 1.000f,        1.000f },
        {"hanning",     0.500f,        1.500f },
        {"hamming",     0.540f,        1.363f },
        {"blackman",    0.420f,        1.727f },
        {"flattop",     0.215568f,     3.770f },
        // Kaiser uses β=8.0 (see compute_window). Matches I0(0)/I0(8)
        // integral; published values: CG ≈ 0.4359, ENBW ≈ 1.666 bins.
        {"kaiser",      0.4359f,       1.666f },
    };
    for (const auto& r : refs) {
        dsp.set_window(r.name);
        EXPECT_NEAR(dsp.coherent_gain(), r.cg,   0.01f) << r.name;
        EXPECT_NEAR(dsp.enbw_bins(),     r.enbw, 0.05f) << r.name;
    }
}

// Tone amplitude is window-independent
TEST(WindowCalibration, BinAlignedToneReadsZeroDBAcrossWindows) {
    // A unit-amplitude (peak = 1) cosine at a bin-aligned frequency
    // should read 0 dB at the spectrum peak no matter which window the
    // user picked - that's the whole point of dividing by coherent_gain.
    constexpr std::size_t N = 4096;
    DSPPipeline dsp({.fft_size = N});
    const auto x = bin_aligned_cosine(N, /*bin=*/100);

    for (const char* name : {"rectangular", "hanning", "hamming",
                             "blackman", "flattop", "kaiser"}) {
        dsp.set_window(name);
        const auto& spec = dsp.process_spectrum({x.data(), N});
        // Tolerance allows for finite-N deviations and bin-edge leakage
        // of windows with wider main lobes. 0.5 dB is generous enough to
        // cover all shipping windows but tight enough to catch a missing
        // coherent-gain divisor (which would shift Hanning by ~6 dB and
        // FlatTop by ~13 dB).
        EXPECT_NEAR(peak_db(std::vector<float>(spec.begin(), spec.end())),
                    0.0f, 0.5f) << "window=" << name;
    }
}

// ENBW affects noise floor, not tone amplitude
TEST(WindowCalibration, EnbwIsConsistentWithSquaredCoeffsRatio) {
    // Sanity check on the ENBW math itself: ENBW = N · sum(w²) / sum(w)².
    // For the rectangular window (all ones), this reduces to N·N/N² = 1.
    // For Hanning, the textbook value is 1.5.
    constexpr std::size_t N = 8192;
    DSPPipeline dsp({.fft_size = N});

    dsp.set_window("rectangular");
    EXPECT_NEAR(dsp.enbw_bins(), 1.0f, 1e-4f);

    dsp.set_window("hanning");
    EXPECT_NEAR(dsp.enbw_bins(), 1.5f, 1e-3f);
}
