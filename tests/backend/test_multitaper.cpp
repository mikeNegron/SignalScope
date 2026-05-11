// Multitaper estimator (sine tapers).
// What we're locking down:
//   - set_taper_count clamps to {1, 2, 4, 8}; default is 1.
//   - K=1 path is bit-identical to the non-MT (single-window) path -
//     the new branch must not change the existing behavior at K=1.
//   - K>1 reduces variance: feed Gaussian noise frame-by-frame and
//     verify the spread of dB values across bins (or across frames at
//     a fixed bin) shrinks as K rises. This is the whole point.
//   - K>1 still reads tone amplitude approximately correctly thanks
//     to the mt_cg calibration.
//   - resize() recomputes the tapers (so an FFT-size change doesn't
//     leave you with the previous size's tapers).

#include <gtest/gtest.h>

#include "dsp/dsp_pipeline.hpp"

#include <cmath>
#include <cstddef>
#include <numbers>
#include <random>
#include <vector>

using signalscope::DSPPipeline;

namespace {

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

// Median of finite values across a slice of bins - used to gauge the
// spread of the noise floor (lower std-dev around the median when K
// rises, since each bin is the average of K independent FFT estimates).
double std_dev_db(const std::vector<float>& spec,
                  std::size_t lo, std::size_t hi)
{
    double sum = 0;
    std::size_t cnt = 0;
    for (std::size_t i = lo; i < hi; i++) {
        if (std::isfinite(spec[i])) { sum += spec[i]; cnt++; }
    }
    if (cnt == 0) return 0.0;
    const double mean = sum / cnt;
    double var = 0;
    for (std::size_t i = lo; i < hi; i++) {
        if (std::isfinite(spec[i])) {
            const double d = spec[i] - mean;
            var += d * d;
        }
    }
    return std::sqrt(var / cnt);
}

} // namespace

// Configuration
TEST(Multitaper, DefaultIsSingleWindow) {
    DSPPipeline dsp({.fft_size = 1024});
    EXPECT_EQ(dsp.taper_count(), 1);
}

TEST(Multitaper, FactorClampsToSupportedSet) {
    DSPPipeline dsp({.fft_size = 1024});
    dsp.set_taper_count(0);  EXPECT_EQ(dsp.taper_count(), 1);
    dsp.set_taper_count(1);  EXPECT_EQ(dsp.taper_count(), 1);
    dsp.set_taper_count(2);  EXPECT_EQ(dsp.taper_count(), 2);
    dsp.set_taper_count(3);  EXPECT_EQ(dsp.taper_count(), 2);  // round down
    dsp.set_taper_count(4);  EXPECT_EQ(dsp.taper_count(), 4);
    dsp.set_taper_count(5);  EXPECT_EQ(dsp.taper_count(), 4);
    dsp.set_taper_count(8);  EXPECT_EQ(dsp.taper_count(), 8);
    dsp.set_taper_count(99); EXPECT_EQ(dsp.taper_count(), 8);
}

// K=1 is unchanged from single-window behavior
TEST(Multitaper, K1MatchesSingleWindow) {
    constexpr std::size_t N = 1024;
    DSPPipeline dsp({.fft_size = N});
    dsp.set_window("hanning");

    const auto x = bin_aligned_cosine(N, /*bin=*/100);

    const auto& s_default = dsp.process_spectrum({x.data(), N});
    const std::vector<float> ref(s_default.begin(), s_default.end());

    // Toggle to K=2 and back to K=1; the K=1 result must equal what
    // we got before - proving that the new branch is dormant at K=1.
    dsp.set_taper_count(2);
    dsp.process_spectrum({x.data(), N});
    dsp.set_taper_count(1);
    const auto& s_back = dsp.process_spectrum({x.data(), N});
    ASSERT_EQ(ref.size(), s_back.size());
    for (std::size_t i = 0; i < ref.size(); i++) {
        EXPECT_FLOAT_EQ(ref[i], s_back[i]) << "bin " << i;
    }
}

// Variance reduction
TEST(Multitaper, NoiseFloorVarianceShrinksWithK) {
    // Drive the pipeline with white Gaussian noise and measure the
    // standard deviation of the dB spectrum across mid-band bins. A
    // single-window FFT of noise has chi-squared statistics; averaging
    // K independent tapers should reduce the dB spread by ~1/sqrt(K).
    constexpr std::size_t N = 4096;
    DSPPipeline dsp({.fft_size = N});
    dsp.set_window("hanning");

    std::mt19937 rng(0x5eed);
    std::normal_distribution<float> noise(0.0f, 0.1f);

    auto make_spec = [&](int K) {
        dsp.set_taper_count(K);
        std::vector<float> input(N);
        for (auto& v : input) v = noise(rng);
        return std::vector<float>(dsp.process_spectrum({input.data(), N}).begin(),
                                  dsp.process_spectrum({input.data(), N}).end());
    };

    // Skip a few hundred bins on each side so the bin-edge taper rolloff
    // doesn't dominate the std-dev.
    constexpr std::size_t lo = 500, hi = N / 2 - 500;

    const double sd_K1 = std_dev_db(make_spec(1), lo, hi);
    const double sd_K4 = std_dev_db(make_spec(4), lo, hi);
    const double sd_K8 = std_dev_db(make_spec(8), lo, hi);

    // Higher K must monotonically reduce variance. Numerical theory:
    // sd_K ≈ sd_1 / sqrt(K). We allow a generous tolerance because the
    // sample size (~N/2 - 1000 bins) is finite and the Gaussian draw
    // itself has variance.
    EXPECT_LT(sd_K4, sd_K1 * 0.85) << "K=4 didn't measurably reduce variance: "
                                   << "sd_K1=" << sd_K1 << " sd_K4=" << sd_K4;
    EXPECT_LT(sd_K8, sd_K4)         << "K=8 didn't beat K=4: "
                                   << "sd_K4=" << sd_K4 << " sd_K8=" << sd_K8;
}

// Tone calibration is approximately preserved
TEST(Multitaper, ToneAmplitudeStaysWithinFewDbAcrossK) {
    // The mt_cg calibration is meant to keep tone amplitude readouts
    // within a few dB of the single-window value as K varies - without
    // it, tones in MT mode read ~30 dB low because individual sine
    // tapers have small coherent gain.
    constexpr std::size_t N = 4096;
    DSPPipeline dsp({.fft_size = N});
    dsp.set_window("hanning");
    const auto x = bin_aligned_cosine(N, /*bin=*/200);

    dsp.set_taper_count(1);
    const float db_K1 = peak_db(std::vector<float>(
        dsp.process_spectrum({x.data(), N}).begin(),
        dsp.process_spectrum({x.data(), N}).end()));

    for (int K : {2, 4, 8}) {
        dsp.set_taper_count(K);
        const float db_K = peak_db(std::vector<float>(
            dsp.process_spectrum({x.data(), N}).begin(),
            dsp.process_spectrum({x.data(), N}).end()));
        EXPECT_NEAR(db_K, db_K1, 6.0f) << "K=" << K
            << " tone amplitude drifted: K1=" << db_K1 << " dB, K=" << db_K;
    }
}

// Resize triggers taper recomputation
TEST(Multitaper, FftSizeChangeRecomputesTapers) {
    DSPPipeline dsp({.fft_size = 256});
    dsp.set_taper_count(4);

    // After resize, the K=4 path must still work - it doesn't if the
    // tapers were sized to the old fft_size and we'd read out of bounds.
    dsp.set_fft_size(1024);
    EXPECT_EQ(dsp.taper_count(), 4);

    std::vector<float> x(1024);
    for (std::size_t i = 0; i < x.size(); i++)
        x[i] = static_cast<float>(std::sin(2.0 * std::numbers::pi_v<float> *
                                           50.0 * i / 1024.0));
    const auto& spec = dsp.process_spectrum(x);
    // Sanity: peak landed in the spectrum, didn't return all-NaN/inf.
    bool any_finite = false;
    for (float v : spec) if (std::isfinite(v) && v > -150.0f) { any_finite = true; break; }
    EXPECT_TRUE(any_finite);
}
