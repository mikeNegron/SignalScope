// Unit tests for util/iq_panel.hpp - the IQ-panel stride clamp used by
// the WS broadcast hot path. Pure function; no sockets, no threads.
//
// The clamp must keep the real-source stride loop in bounds across the
// full range of hop sizes the backend can produce, including the
// pathological tiny-FFT / high-overlap case where hop_size < 2.

#include <gtest/gtest.h>

#include "util/iq_panel.hpp"

#include <cstddef>

using signalscope::clamp_iq_len;

// hop_size < 2 has no representable safe length: stride reads i*2+1 so
// any positive iq_len would index out of bounds. Must clamp to zero.
TEST(ClampIqLen, ZeroHopReturnsZero) {
    EXPECT_EQ(clamp_iq_len(0,   0), 0u);
    EXPECT_EQ(clamp_iq_len(1,   0), 0u);
    EXPECT_EQ(clamp_iq_len(512, 0), 0u);
}

TEST(ClampIqLen, HopOfOneReturnsZero) {
    EXPECT_EQ(clamp_iq_len(0,   1), 0u);
    EXPECT_EQ(clamp_iq_len(1,   1), 0u);
    EXPECT_EQ(clamp_iq_len(512, 1), 0u);
}

// hop_size == 2 - exactly one (I, Q) pair fits.
TEST(ClampIqLen, HopOfTwoAllowsAtMostOne) {
    EXPECT_EQ(clamp_iq_len(0,   2), 0u);
    EXPECT_EQ(clamp_iq_len(1,   2), 1u);
    EXPECT_EQ(clamp_iq_len(512, 2), 1u);
}

// Normal hop sizes: clamp at hop/2 when requested is larger, otherwise
// pass requested through unchanged.
TEST(ClampIqLen, LargeHopClampsAtHalf) {
    EXPECT_EQ(clamp_iq_len(512,  64),   32u);
    EXPECT_EQ(clamp_iq_len(512,  1024), 512u); // requested wins
    EXPECT_EQ(clamp_iq_len(2000, 1024), 512u); // hop/2 wins
}

TEST(ClampIqLen, RequestSmallerThanHalfHopPassesThrough) {
    EXPECT_EQ(clamp_iq_len(16,  4096), 16u);
    EXPECT_EQ(clamp_iq_len(100, 4096), 100u);
}

TEST(ClampIqLen, BoundaryAtExactlyHalf) {
    // requested == hop/2 - exactly fits, no clamp needed.
    EXPECT_EQ(clamp_iq_len(64, 128), 64u);
}

// constexpr usability - clamp must be evaluable at compile time so it
// can sit in a header without ODR concerns.
TEST(ClampIqLen, IsConstexpr) {
    constexpr std::size_t a = clamp_iq_len(512, 1024);
    constexpr std::size_t b = clamp_iq_len(512, 1);
    static_assert(a == 512u);
    static_assert(b == 0u);
    SUCCEED() << "constexpr evaluability verified via static_assert above";
}
