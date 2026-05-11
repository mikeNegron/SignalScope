// DSPPipeline overlap (FFT window stride) tests.
// Locking down the contract:
//   - Default overlap is 1 (no overlap); hop_size() == fft_size.
//   - set_overlap(2) yields hop_size = fft_size/2; set_overlap(4) yields
//     fft_size/4. set_overlap(3) is rounded down to 2 (only powers of two
//     are supported on the wire).
//   - With overlap engaged, two consecutive process_spectrum calls produce
//     bit-identical FFT output to a single non-overlapped call covering the
//     same time-domain window - i.e., the slide buffer correctly reuses
//     the retained portion of the previous window.
//   - resize() (i.e. set_fft_size) clears the slide buffer so an
//     fft-size change can't carry stale samples through.

#include <gtest/gtest.h>

#include "dsp/dsp_pipeline.hpp"

#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

using signalscope::DSPPipeline;

// Configuration
TEST(DSPPipelineOverlap, DefaultOverlapFactorIsOne) {
    DSPPipeline dsp({.fft_size = 1024});
    EXPECT_EQ(dsp.overlap_factor(), 1);
    EXPECT_EQ(dsp.hop_size(), 1024u);
}

TEST(DSPPipelineOverlap, FactorClampsToSupportedSet) {
    DSPPipeline dsp({.fft_size = 1024});
    dsp.set_overlap(0);  EXPECT_EQ(dsp.overlap_factor(), 1);
    dsp.set_overlap(1);  EXPECT_EQ(dsp.overlap_factor(), 1);
    dsp.set_overlap(2);  EXPECT_EQ(dsp.overlap_factor(), 2);
    dsp.set_overlap(3);  EXPECT_EQ(dsp.overlap_factor(), 2);  // rounds down
    dsp.set_overlap(4);  EXPECT_EQ(dsp.overlap_factor(), 4);
    dsp.set_overlap(7);  EXPECT_EQ(dsp.overlap_factor(), 4);  // rounds down
    dsp.set_overlap(8);  EXPECT_EQ(dsp.overlap_factor(), 8);
    dsp.set_overlap(15); EXPECT_EQ(dsp.overlap_factor(), 8);  // rounds down
    dsp.set_overlap(16); EXPECT_EQ(dsp.overlap_factor(), 16);
    dsp.set_overlap(64); EXPECT_EQ(dsp.overlap_factor(), 16); // clamps at 16
}

TEST(DSPPipelineOverlap, HopSizeFollowsFactor) {
    DSPPipeline dsp({.fft_size = 4096});
    dsp.set_overlap(1); EXPECT_EQ(dsp.hop_size(), 4096u);
    dsp.set_overlap(2); EXPECT_EQ(dsp.hop_size(), 2048u);
    dsp.set_overlap(4); EXPECT_EQ(dsp.hop_size(), 1024u);
}

// Slide buffer correctness
TEST(DSPPipelineOverlap, OverlapMatchesNonOverlapWhenWindowsSpanSameSamples) {
    // Build a known signal long enough to feed two hops into the overlap
    // path. After two hop-sized calls (50 % overlap), the slide buffer
    // should hold samples [hop, hop + fft_size). Compare its FFT output
    // against a single non-overlap call given exactly those samples.
    constexpr std::size_t N = 1024;
    constexpr std::size_t hop = N / 2;
    constexpr float fs = 48000.0f;

    // Two sinusoids so the spectrum has identifiable peaks.
    std::vector<float> signal(N + hop);
    for (std::size_t i = 0; i < signal.size(); i++) {
        const float t = static_cast<float>(i) / fs;
        signal[i] = std::sin(2.0f * std::numbers::pi_v<float> * 1000.0f * t)
                  + 0.5f * std::sin(2.0f * std::numbers::pi_v<float> * 5000.0f * t);
    }

    // Reference: a single non-overlap call on samples [hop, hop + N).
    DSPPipeline ref({.fft_size = N});
    ref.set_window("rectangular");
    const auto& ref_spec = ref.process_spectrum({signal.data() + hop, N});
    const std::vector<float> reference(ref_spec.begin(), ref_spec.end());

    // Overlap path: feed two hops in sequence. The first call seeds the
    // slide buffer (back half is zero), so its output is meaningless. The
    // second call's slide buffer contains [signal[0..hop)] at the back
    // followed by [signal[hop..2hop)] at the front… which is samples
    // [0, N) of the input. Wait - that's NOT the same window as the
    // reference. To align them we need to feed the prior hop FIRST so
    // the slide carries it, then feed the next hop and compare.
    DSPPipeline ovr({.fft_size = N});
    ovr.set_window("rectangular");
    ovr.set_overlap(2);
    ovr.process_spectrum({signal.data(), hop});  // primes the back half
    const auto& ovr_spec = ovr.process_spectrum({signal.data() + hop, hop});
    // After this call the slide buffer's contents are
    // [signal[0..hop), signal[hop..2hop)] = signal[0..N). Compare to a
    // separate non-overlap reference on those same samples.
    DSPPipeline ref2({.fft_size = N});
    ref2.set_window("rectangular");
    const auto& ref2_spec = ref2.process_spectrum({signal.data(), N});
    ASSERT_EQ(ovr_spec.size(), ref2_spec.size());
    for (std::size_t i = 0; i < ovr_spec.size(); i++) {
        EXPECT_NEAR(ovr_spec[i], ref2_spec[i], 1e-3f) << "bin " << i;
    }
}

TEST(DSPPipelineOverlap, FirstFrameAfterToggleSeesOnlyFreshSamples) {
    // Toggling overlap on must clear the slide buffer - otherwise the
    // first frame would mix whatever was in there before.
    constexpr std::size_t N = 256;
    DSPPipeline dsp({.fft_size = N});
    dsp.set_window("rectangular");

    // Run a few non-overlap frames so internal state has values.
    std::vector<float> ones(N, 1.0f);
    dsp.process_spectrum({ones.data(), N});

    // Engage 50 % overlap. Slide buffer should be zeroed.
    dsp.set_overlap(2);

    // First overlap frame: hop fresh samples of zeros. The other half
    // (back of the slide buffer) is zero from the reset, so the FFT
    // input is all zeros -> spectrum is the floor (-200 dBFS) everywhere.
    std::vector<float> zeros(N / 2, 0.0f);
    const auto& spec = dsp.process_spectrum(zeros);
    for (std::size_t i = 0; i < spec.size(); i++) {
        EXPECT_LE(spec[i], -100.0f) << "bin " << i << " carries non-zero "
                                       "signal from before the overlap toggle";
    }
}

TEST(DSPPipelineOverlap, SetFftSizeResetsSlideBuffer) {
    // Resizing the FFT must reset the slide buffer too - otherwise samples
    // sized for the old fft_size would land in the new buffer at the wrong
    // offsets.
    DSPPipeline dsp({.fft_size = 256});
    dsp.set_window("rectangular");
    dsp.set_overlap(2);
    std::vector<float> ones(128, 1.0f);
    dsp.process_spectrum(ones);

    dsp.set_fft_size(1024);
    // overlap_factor stays at 2 across the resize; hop = 1024 / 2 = 512.
    EXPECT_EQ(dsp.overlap_factor(), 2);
    EXPECT_EQ(dsp.hop_size(), 512u);

    // Feed one frame of zeros at the new size. Output should be at the
    // floor - no carry-over from the 256-size pipeline.
    std::vector<float> zeros(512, 0.0f);
    const auto& spec = dsp.process_spectrum(zeros);
    for (std::size_t i = 0; i < spec.size(); i++) {
        EXPECT_LE(spec[i], -100.0f) << "bin " << i;
    }
}

// Complex path mirrors the real path
TEST(DSPPipelineOverlap, ComplexOverlapMatchesNonOverlap) {
    constexpr std::size_t N = 512;
    constexpr std::size_t hop = N / 2;
    constexpr float fs = 48000.0f;

    std::vector<float> re(N + hop), im(N + hop);
    for (std::size_t i = 0; i < re.size(); i++) {
        const float t = static_cast<float>(i) / fs;
        const float w = 2.0f * std::numbers::pi_v<float> * 2000.0f * t;
        re[i] = std::cos(w);
        im[i] = std::sin(w);
    }

    DSPPipeline ovr({.fft_size = N});
    ovr.set_window("rectangular");
    ovr.set_overlap(2);
    ovr.process_spectrum_complex({re.data(), hop}, {im.data(), hop});
    const auto& ovr_spec = ovr.process_spectrum_complex(
        {re.data() + hop, hop}, {im.data() + hop, hop});

    DSPPipeline ref({.fft_size = N});
    ref.set_window("rectangular");
    const auto& ref_spec = ref.process_spectrum_complex(
        {re.data(), N}, {im.data(), N});

    ASSERT_EQ(ovr_spec.size(), ref_spec.size());
    for (std::size_t i = 0; i < ovr_spec.size(); i++) {
        EXPECT_NEAR(ovr_spec[i], ref_spec[i], 1e-3f) << "bin " << i;
    }
}
