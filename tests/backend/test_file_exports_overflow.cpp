// Verifies that the WAV/AIFF exporters refuse to write a file whose
// data section would overflow the uint32_t chunk-size field. Prior
// behavior was a silent truncation that produced a malformed file
// loadable only by lenient decoders.

#include <gtest/gtest.h>

#include "util/file_exports.hpp"

#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <span>

namespace fx = signalscope::exports;

// We cannot actually allocate a 4 GB buffer in a test. The early-return
// guard runs BEFORE any indexing into the span, so it's safe to construct
// a span with a lied-about size as long as the writer doesn't index it.
// std::span just stores (ptr, count); construction is well-defined
// regardless of whether ptr+count points past the real allocation.
// The contract under test: data.size() > cap -> return false, no file
// touched (the guard fires before fopen).
TEST(FileExportsOverflow, WavRefusesOversizedFloat32Payload) {
    constexpr std::size_t kCap = fx::kMaxWavFloat32Samples;
    const float dummy = 0.0f;
    const auto fake = std::span<const float>(&dummy, kCap + 1);
    EXPECT_FALSE(fx::write_wav_float32("/tmp/signalscope_oversized_wav_test.wav",
                                       fake, 48000.0f))
        << "WAV writer must reject payloads where data.size()*sizeof(float) > UINT32_MAX";
}

TEST(FileExportsOverflow, WavAcceptsSmallPayload) {
    // At-the-cap should succeed (well, the guard should not fire). To
    // avoid actually writing a 4 GB file in the test, we don't probe
    // exactly at the boundary - instead, confirm an in-range size with
    // a tiny real buffer is accepted.
    const float dummy[4] = {0.0f, 0.1f, -0.1f, 0.0f};
    EXPECT_TRUE(fx::write_wav_float32("/tmp/signalscope_small_wav_test.wav",
                                      std::span<const float>{dummy, 4},
                                      48000.0f));
    std::remove("/tmp/signalscope_small_wav_test.wav");
}

TEST(FileExportsOverflow, AiffRefusesOversizedInt16Payload) {
    constexpr std::size_t kCap = fx::kMaxAiffInt16Samples;
    const float dummy = 0.0f;
    const auto fake = std::span<const float>(&dummy, kCap + 1);
    EXPECT_FALSE(fx::write_aiff_int16("/tmp/signalscope_oversized_aiff_test.aiff",
                                      fake, 48000.0f))
        << "AIFF writer must reject payloads where data.size()*2 > UINT32_MAX";
}

TEST(FileExportsOverflow, AiffAcceptsSmallPayload) {
    const float dummy[4] = {0.0f, 0.1f, -0.1f, 0.0f};
    EXPECT_TRUE(fx::write_aiff_int16("/tmp/signalscope_small_aiff_test.aiff",
                                     std::span<const float>{dummy, 4},
                                     48000.0f));
    std::remove("/tmp/signalscope_small_aiff_test.aiff");
}
