#pragma once
#include "dsp/simd_ops/common.hpp"

namespace signalscope::simd {

// Reduce: min and max of src[0..N).
inline void min_max_scalar(const float* src, std::size_t N,
                           float& out_min, float& out_max) noexcept
{
    float mn =  std::numeric_limits<float>::infinity();
    float mx = -std::numeric_limits<float>::infinity();
    for (std::size_t i = 0; i < N; i++) {
        if (src[i] < mn) mn = src[i];
        if (src[i] > mx) mx = src[i];
    }
    out_min = mn; out_max = mx;
}

// Reduce: min and max of src[0..N), EXCLUDING any value v with
// v <= ignore_le (treated as a low-sentinel) or v >= ignore_ge
// (treated as a high-sentinel). Strict against the ignore threshold —
// equal-to is also excluded.
//
// If every value is excluded, out_min stays at +infinity and out_max at
// -infinity. Callers should check `!std::isfinite(out_min) ||
// !std::isfinite(out_max) || out_max <= out_min` to detect the
// "all sentinels / empty range" case and fall back to a default range.
inline void min_max_filtered_scalar(const float* src, std::size_t N,
                                    float ignore_le, float ignore_ge,
                                    float& out_min, float& out_max) noexcept
{
    float mn =  std::numeric_limits<float>::infinity();
    float mx = -std::numeric_limits<float>::infinity();
    for (std::size_t i = 0; i < N; i++) {
        const float v = src[i];
        if (v < mn && v > ignore_le) mn = v;
        if (v > mx && v < ignore_ge) mx = v;
    }
    out_min = mn; out_max = mx;
}

#if SIGNALSCOPE_HAS_AVX2
inline void min_max_avx2(const float* src, std::size_t N,
                         float& out_min, float& out_max) noexcept
{
    if (N == 0) {
        out_min =  std::numeric_limits<float>::infinity();
        out_max = -std::numeric_limits<float>::infinity();
        return;
    }
    constexpr std::size_t lanes = 8;
    __m256 vmn = _mm256_set1_ps( std::numeric_limits<float>::infinity());
    __m256 vmx = _mm256_set1_ps(-std::numeric_limits<float>::infinity());
    const std::size_t n_full = N & ~(lanes - 1);
    for (std::size_t i = 0; i < n_full; i += lanes) {
        __m256 v = _mm256_loadu_ps(src + i);
        vmn = _mm256_min_ps(vmn, v);
        vmx = _mm256_max_ps(vmx, v);
    }
    // Horizontal reduce.
    alignas(32) float bmn[8], bmx[8];
    _mm256_store_ps(bmn, vmn);
    _mm256_store_ps(bmx, vmx);
    float mn = bmn[0], mx = bmx[0];
    for (int j = 1; j < 8; j++) {
        if (bmn[j] < mn) mn = bmn[j];
        if (bmx[j] > mx) mx = bmx[j];
    }
    for (std::size_t i = n_full; i < N; i++) {
        if (src[i] < mn) mn = src[i];
        if (src[i] > mx) mx = src[i];
    }
    out_min = mn; out_max = mx;
}

// Sentinel-aware vectorized variant. Per lane, values where v <= ignore_le
// are replaced with +infinity (no-op for the min reduction); values where
// v >= ignore_ge are replaced with -infinity (no-op for the max reduction).
inline void min_max_filtered_avx2(const float* src, std::size_t N,
                                  float ignore_le, float ignore_ge,
                                  float& out_min, float& out_max) noexcept
{
    if (N == 0) {
        out_min =  std::numeric_limits<float>::infinity();
        out_max = -std::numeric_limits<float>::infinity();
        return;
    }
    const __m256 vinf       = _mm256_set1_ps( std::numeric_limits<float>::infinity());
    const __m256 vneg_inf   = _mm256_set1_ps(-std::numeric_limits<float>::infinity());
    const __m256 vignore_le = _mm256_set1_ps(ignore_le);
    const __m256 vignore_ge = _mm256_set1_ps(ignore_ge);

    constexpr std::size_t lanes = 8;
    __m256 vmn = vinf;
    __m256 vmx = vneg_inf;
    const std::size_t n_full = N & ~(lanes - 1);
    for (std::size_t i = 0; i < n_full; i += lanes) {
        __m256 v = _mm256_loadu_ps(src + i);
        // For min: lanes where v <= ignore_le become +inf (no-op for min).
        __m256 mn_skip = _mm256_cmp_ps(v, vignore_le, _CMP_LE_OS);
        __m256 v_for_min = _mm256_blendv_ps(v, vinf, mn_skip);
        // For max: lanes where v >= ignore_ge become -inf (no-op for max).
        __m256 mx_skip = _mm256_cmp_ps(v, vignore_ge, _CMP_GE_OS);
        __m256 v_for_max = _mm256_blendv_ps(v, vneg_inf, mx_skip);

        vmn = _mm256_min_ps(vmn, v_for_min);
        vmx = _mm256_max_ps(vmx, v_for_max);
    }
    // Horizontal reduce.
    alignas(32) float bmn[8], bmx[8];
    _mm256_store_ps(bmn, vmn);
    _mm256_store_ps(bmx, vmx);
    float mn = bmn[0], mx = bmx[0];
    for (int j = 1; j < 8; j++) {
        if (bmn[j] < mn) mn = bmn[j];
        if (bmx[j] > mx) mx = bmx[j];
    }
    // Scalar tail with the same exclusion logic.
    for (std::size_t i = n_full; i < N; i++) {
        const float v = src[i];
        if (v < mn && v > ignore_le) mn = v;
        if (v > mx && v < ignore_ge) mx = v;
    }
    out_min = mn; out_max = mx;
}
#endif // SIGNALSCOPE_HAS_AVX2

#if SIGNALSCOPE_HAS_NEON
inline void min_max_neon(const float* src, std::size_t N,
                         float& out_min, float& out_max) noexcept
{
    if (N == 0) {
        out_min =  std::numeric_limits<float>::infinity();
        out_max = -std::numeric_limits<float>::infinity();
        return;
    }
    constexpr std::size_t lanes = 4;
    float32x4_t vmn = vdupq_n_f32( std::numeric_limits<float>::infinity());
    float32x4_t vmx = vdupq_n_f32(-std::numeric_limits<float>::infinity());
    const std::size_t n_full = N & ~(lanes - 1);
    for (std::size_t i = 0; i < n_full; i += lanes) {
        float32x4_t v = vld1q_f32(src + i);
        vmn = vminq_f32(vmn, v);
        vmx = vmaxq_f32(vmx, v);
    }
    // Horizontal reduce (vminvq is aarch64-only; emulate for portability).
    alignas(16) float bmn[4], bmx[4];
    vst1q_f32(bmn, vmn);
    vst1q_f32(bmx, vmx);
    float mn = bmn[0], mx = bmx[0];
    for (int j = 1; j < 4; j++) {
        if (bmn[j] < mn) mn = bmn[j];
        if (bmx[j] > mx) mx = bmx[j];
    }
    for (std::size_t i = n_full; i < N; i++) {
        if (src[i] < mn) mn = src[i];
        if (src[i] > mx) mx = src[i];
    }
    out_min = mn; out_max = mx;
}

// Sentinel-aware vectorized variant; see the AVX2 version for the
// substitution trick (excluded lanes -> ±infinity neutrals).
inline void min_max_filtered_neon(const float* src, std::size_t N,
                                  float ignore_le, float ignore_ge,
                                  float& out_min, float& out_max) noexcept
{
    if (N == 0) {
        out_min =  std::numeric_limits<float>::infinity();
        out_max = -std::numeric_limits<float>::infinity();
        return;
    }
    const float32x4_t vinf       = vdupq_n_f32( std::numeric_limits<float>::infinity());
    const float32x4_t vneg_inf   = vdupq_n_f32(-std::numeric_limits<float>::infinity());
    const float32x4_t vignore_le = vdupq_n_f32(ignore_le);
    const float32x4_t vignore_ge = vdupq_n_f32(ignore_ge);

    constexpr std::size_t lanes = 4;
    float32x4_t vmn = vinf;
    float32x4_t vmx = vneg_inf;
    const std::size_t n_full = N & ~(lanes - 1);
    for (std::size_t i = 0; i < n_full; i += lanes) {
        float32x4_t v = vld1q_f32(src + i);
        // vcleq_f32(a, b) = a <= b -> mask of all-1s in qualifying lanes.
        uint32x4_t mn_skip = vcleq_f32(v, vignore_le);
        float32x4_t v_for_min = vbslq_f32(mn_skip, vinf, v);
        uint32x4_t mx_skip = vcgeq_f32(v, vignore_ge);
        float32x4_t v_for_max = vbslq_f32(mx_skip, vneg_inf, v);

        vmn = vminq_f32(vmn, v_for_min);
        vmx = vmaxq_f32(vmx, v_for_max);
    }
    alignas(16) float bmn[4], bmx[4];
    vst1q_f32(bmn, vmn);
    vst1q_f32(bmx, vmx);
    float mn = bmn[0], mx = bmx[0];
    for (int j = 1; j < 4; j++) {
        if (bmn[j] < mn) mn = bmn[j];
        if (bmx[j] > mx) mx = bmx[j];
    }
    for (std::size_t i = n_full; i < N; i++) {
        const float v = src[i];
        if (v < mn && v > ignore_le) mn = v;
        if (v > mx && v < ignore_ge) mx = v;
    }
    out_min = mn; out_max = mx;
}
#endif // SIGNALSCOPE_HAS_NEON

inline void min_max(const float* src, std::size_t N,
                    float& out_min, float& out_max) noexcept {
#if SIGNALSCOPE_HAS_AVX2
    min_max_avx2(src, N, out_min, out_max);
#elif SIGNALSCOPE_HAS_NEON
    min_max_neon(src, N, out_min, out_max);
#else
    min_max_scalar(src, N, out_min, out_max);
#endif
}

inline void min_max_filtered(const float* src, std::size_t N,
                             float ignore_le, float ignore_ge,
                             float& out_min, float& out_max) noexcept {
#if SIGNALSCOPE_HAS_AVX2
    min_max_filtered_avx2(src, N, ignore_le, ignore_ge, out_min, out_max);
#elif SIGNALSCOPE_HAS_NEON
    min_max_filtered_neon(src, N, ignore_le, ignore_ge, out_min, out_max);
#else
    min_max_filtered_scalar(src, N, ignore_le, ignore_ge, out_min, out_max);
#endif
}

} // namespace signalscope::simd
