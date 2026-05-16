// Correctness tests for simd_ops.hpp. Each test compares the public
// dispatch entry point against the scalar reference on a fixed
// deterministic input. The tolerance for magnitude_db accounts for the
// log10 polynomial approximation; the other three functions are exact.

#include <gtest/gtest.h>

#include "dsp/simd_ops.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace signalscope::simd;

namespace {
// Deterministic PRNG — same one the bench uses.
struct Xoshiro128 {
    uint32_t s0 = 0x9E3779B9, s1 = 0x243F6A88, s2 = 0xB7E15162, s3 = 0xBB67AE85;
    uint32_t next() {
        const uint32_t r = s0 + s3;
        const uint32_t t = s1 << 9;
        s2 ^= s0; s3 ^= s1; s1 ^= s2; s0 ^= s3;
        s2 ^= t;
        s3 = (s3 << 11) | (s3 >> 21);
        return r;
    }
    float unit() { return (static_cast<int32_t>(next()) * (1.0f / 2147483648.0f)); }
};

std::vector<float> rand_vec(std::size_t n, uint32_t seed, float scale = 1.0f) {
    Xoshiro128 r; r.s0 ^= seed;
    std::vector<float> v(n);
    for (auto& x : v) x = r.unit() * scale;
    return v;
}
} // namespace

TEST(SimdOps, MagnitudeDbMatchesScalar) {
    // 1e-3 dB tolerance accommodates the Mercator-series log10 truncation:
    // for u ∈ [0, 1/3] the next-term bound is ~5.7e-4 dB. For comparison,
    // typical screen rendering anchors at 0.1 dB granularity.
    constexpr float kTol = 1e-3f;
    constexpr std::size_t halfN = 4096;

    auto re_im = rand_vec(2 * halfN, 0x1234, 0.5f);
    std::vector<float> ref(halfN), got(halfN);

    magnitude_db_scalar(re_im.data(), ref.data(), halfN, 2.0f / halfN);
    magnitude_db       (re_im.data(), got.data(), halfN, 2.0f / halfN);

    for (std::size_t i = 0; i < halfN; i++) {
        ASSERT_NEAR(got[i], ref[i], kTol) << "bin " << i;
    }
}

TEST(SimdOps, MagnitudeDbFloorsTinyValues) {
    // Inputs whose magnitude falls below the 1e-10 epsilon must clamp
    // to exactly -200, not a tiny finite number from the log approx.
    constexpr std::size_t halfN = 64;
    std::vector<float> re_im(2 * halfN, 0.0f);  // exact zeros
    std::vector<float> out(halfN, 0.0f);
    magnitude_db(re_im.data(), out.data(), halfN, 1.0f);
    for (auto v : out) ASSERT_FLOAT_EQ(v, -200.0f);
}

TEST(SimdOps, MagnitudeDbHandlesUnalignedTail) {
    // halfN that is not a multiple of any SIMD lane width (4 or 8).
    // Catches a tail-fallthrough bug where the SIMD body writes past the
    // intended range or the scalar tail skips elements.
    constexpr float kTol = 1e-3f;
    constexpr std::size_t halfN = 4093;  // prime; not a multiple of 4 or 8

    auto re_im = rand_vec(2 * halfN, 0x5A5A, 0.5f);
    std::vector<float> ref(halfN), got(halfN);

    magnitude_db_scalar(re_im.data(), ref.data(), halfN, 2.0f / halfN);
    magnitude_db       (re_im.data(), got.data(), halfN, 2.0f / halfN);

    for (std::size_t i = 0; i < halfN; i++) {
        ASSERT_NEAR(got[i], ref[i], kTol) << "bin " << i;
    }
}

TEST(SimdOps, WindowApplyMatchesScalar) {
    constexpr std::size_t N = 8192;
    auto in  = rand_vec(N, 0xAAAA);
    auto win = rand_vec(N, 0xBBBB);
    std::vector<float> ref(N), got(N);

    window_apply_scalar(in.data(), win.data(), ref.data(), N, 0.7f);
    window_apply       (in.data(), win.data(), got.data(), N, 0.7f);

    for (std::size_t i = 0; i < N; i++) {
        ASSERT_FLOAT_EQ(got[i], ref[i]) << "bin " << i;
    }
}

TEST(SimdOps, WindowApplyHandlesUnalignedTail) {
    // SIMD lanes are 8-wide (AVX2) or 4-wide (NEON); a size that isn't
    // a multiple of the lane count exercises the scalar-tail path.
    constexpr std::size_t N = 8191; // prime-ish, definitely not a multiple
    auto in  = rand_vec(N, 0xC0DE);
    auto win = rand_vec(N, 0xBEEF);
    std::vector<float> ref(N), got(N);

    window_apply_scalar(in.data(), win.data(), ref.data(), N, 1.25f);
    window_apply       (in.data(), win.data(), got.data(), N, 1.25f);

    for (std::size_t i = 0; i < N; i++) {
        ASSERT_FLOAT_EQ(got[i], ref[i]) << "bin " << i;
    }
}

TEST(SimdOps, MaxInplaceMatchesScalar) {
    constexpr std::size_t N = 4096;
    auto a = rand_vec(N, 0x1111);
    auto b = rand_vec(N, 0x2222);
    std::vector<float> ref = a;
    std::vector<float> got = a;

    max_inplace_scalar(b.data(), ref.data(), N);
    max_inplace       (b.data(), got.data(), N);

    for (std::size_t i = 0; i < N; i++) {
        ASSERT_FLOAT_EQ(got[i], ref[i]) << "bin " << i;
    }
}

TEST(SimdOps, MaxInplaceHandlesUnalignedTail) {
    constexpr std::size_t N = 4093;  // prime; not a multiple of 4 or 8
    auto a = rand_vec(N, 0x7777);
    auto b = rand_vec(N, 0x8888);
    std::vector<float> ref = a;
    std::vector<float> got = a;

    max_inplace_scalar(b.data(), ref.data(), N);
    max_inplace       (b.data(), got.data(), N);

    for (std::size_t i = 0; i < N; i++) {
        ASSERT_FLOAT_EQ(got[i], ref[i]) << "bin " << i;
    }
}

TEST(SimdOps, MinMaxMatchesScalar) {
    constexpr std::size_t N = 8192;
    auto v = rand_vec(N, 0x3333, 200.0f);
    float ref_min, ref_max, got_min, got_max;

    min_max_scalar(v.data(), N, ref_min, ref_max);
    min_max       (v.data(), N, got_min, got_max);

    ASSERT_FLOAT_EQ(got_min, ref_min);
    ASSERT_FLOAT_EQ(got_max, ref_max);
}

TEST(SimdOps, MinMaxHandlesUnalignedTail) {
    constexpr std::size_t N = 4093;  // tail-exercising size
    auto v = rand_vec(N, 0x4444, 100.0f);
    float ref_min, ref_max, got_min, got_max;
    min_max_scalar(v.data(), N, ref_min, ref_max);
    min_max       (v.data(), N, got_min, got_max);
    ASSERT_FLOAT_EQ(got_min, ref_min);
    ASSERT_FLOAT_EQ(got_max, ref_max);
}

TEST(SimdOps, MinMaxFilteredMatchesScalar) {
    // Sentinel scenario: most bins in [-200, 10], a few at ±1e30 sentinels.
    constexpr std::size_t N = 8192;
    auto v = rand_vec(N, 0x9999, 100.0f);  // ~[-100, 100] dB-like range
    // Sprinkle sentinels.
    v[100]    = -1e30f;
    v[2000]   = -1e30f;
    v[N - 50] =  1e30f;
    v[N - 1]  =  1e30f;
    float ref_min, ref_max, got_min, got_max;
    min_max_filtered_scalar(v.data(), N, -1e30f, 1e30f, ref_min, ref_max);
    min_max_filtered       (v.data(), N, -1e30f, 1e30f, got_min, got_max);
    ASSERT_FLOAT_EQ(got_min, ref_min);
    ASSERT_FLOAT_EQ(got_max, ref_max);
    // Sanity: sentinels really were excluded.
    ASSERT_GT(got_min, -1e29f);
    ASSERT_LT(got_max,  1e29f);
}

TEST(SimdOps, MinMaxFilteredHandlesUnalignedTail) {
    constexpr std::size_t N = 4093;  // prime; not a multiple of 4 or 8
    auto v = rand_vec(N, 0xABCD, 50.0f);
    v[N - 3] = -1e30f;  // sentinel in the scalar-tail region
    v[N - 2] =  1e30f;
    float ref_min, ref_max, got_min, got_max;
    min_max_filtered_scalar(v.data(), N, -1e30f, 1e30f, ref_min, ref_max);
    min_max_filtered       (v.data(), N, -1e30f, 1e30f, got_min, got_max);
    ASSERT_FLOAT_EQ(got_min, ref_min);
    ASSERT_FLOAT_EQ(got_max, ref_max);
}

TEST(SimdOps, MinMaxFilteredAllSentinels) {
    // Every value is the low sentinel — min has no eligible candidates
    // so out_min stays at +infinity; out_max ends up at the sentinel
    // itself (a low sentinel is not excluded from the max side). The
    // caller's sanity check `!isfinite(min) || max <= min` still fires,
    // which is the actual contract being defended here. Mirror with
    // a high-sentinel-only case and confirm both fall back.
    constexpr std::size_t N = 128;
    std::vector<float> v_low(N, -1e30f);
    float lo_min, lo_max;
    min_max_filtered(v_low.data(), N, -1e30f, 1e30f, lo_min, lo_max);
    ASSERT_TRUE(std::isinf(lo_min) && lo_min > 0);
    ASSERT_TRUE(!std::isfinite(lo_min) || !std::isfinite(lo_max) || lo_max <= lo_min);

    std::vector<float> v_high(N, 1e30f);
    float hi_min, hi_max;
    min_max_filtered(v_high.data(), N, -1e30f, 1e30f, hi_min, hi_max);
    ASSERT_TRUE(std::isinf(hi_max) && hi_max < 0);
    ASSERT_TRUE(!std::isfinite(hi_min) || !std::isfinite(hi_max) || hi_max <= hi_min);

    // Cross-check against the scalar reference for byte-identical behavior.
    float ref_lo_min, ref_lo_max, ref_hi_min, ref_hi_max;
    min_max_filtered_scalar(v_low.data(),  N, -1e30f, 1e30f, ref_lo_min, ref_lo_max);
    min_max_filtered_scalar(v_high.data(), N, -1e30f, 1e30f, ref_hi_min, ref_hi_max);
    ASSERT_FLOAT_EQ(lo_min, ref_lo_min);
    ASSERT_FLOAT_EQ(lo_max, ref_lo_max);
    ASSERT_FLOAT_EQ(hi_min, ref_hi_min);
    ASSERT_FLOAT_EQ(hi_max, ref_hi_max);
}
