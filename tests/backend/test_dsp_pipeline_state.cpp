// Verifies that DSPPipeline::set_avg_count immediately discards the
// stale averaging history. Without the fix, reducing avg_count from
// e.g. 8 to 2 would still average the old 8 frames with the new ones
// for ~6 frames before draining, producing visibly wrong dB values
// during the transition.

#include <gtest/gtest.h>

#include "dsp/dsp_pipeline.hpp"

#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

using signalscope::DSPPipeline;
using signalscope::WindowFunction;

namespace {

// Generate a deterministic unit-amplitude sinusoid at ~midband, used
// as the "current frame" after the avg_count change.
std::vector<float> tone(std::size_t N, float freq_norm) {
    std::vector<float> v(N);
    for (std::size_t i = 0; i < N; i++) {
        v[i] = std::sin(2.0f * std::numbers::pi_v<float> * freq_norm
                        * static_cast<float>(i));
    }
    return v;
}

}  // namespace

TEST(DSPPipelineState, SetAvgCountDropsStaleHistory) {
    constexpr std::size_t N = 4096;
    DSPPipeline dsp{{N, 48000.0f, WindowFunction::Hanning}};
    dsp.set_avg_count(8);

    // Push 8 frames of one input so the history fills with that signal.
    const auto a = tone(N, 0.05f);
    for (int i = 0; i < 8; i++) dsp.process_spectrum(a);

    // Now switch to a different signal AND immediately drop avg_count to 2.
    // After the change, the next frame's output must reflect ONLY signal b
    // (since avg_history should be empty and only b is in the window) —
    // not a blend with the leftover 7 frames of a.
    dsp.set_avg_count(2);
    const auto b = tone(N, 0.15f);
    const auto& spec_b = dsp.process_spectrum(b);

    // Reference: a fresh pipeline with avg_count=2 fed the same signal b.
    // With a single frame in history, the running-average output equals
    // the raw frame, so this gives us the "correct" single-frame answer.
    DSPPipeline dsp_ref{{N, 48000.0f, WindowFunction::Hanning}};
    dsp_ref.set_avg_count(2);
    const auto& spec_ref = dsp_ref.process_spectrum(b);

    ASSERT_EQ(spec_b.size(), spec_ref.size());
    for (std::size_t i = 0; i < spec_b.size(); i++) {
        // Identity check after the fix — stale history is gone, so the
        // single-frame output equals the reference bin-for-bin.
        EXPECT_FLOAT_EQ(spec_b[i], spec_ref[i]) << "bin " << i;
    }
}
