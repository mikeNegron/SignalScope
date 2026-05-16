#pragma once
#include "dsp/simd_ops/common.hpp"

namespace signalscope::simd {

// dst[i] = max(dst[i], src[i])
inline void max_inplace_scalar(const float* src, float* dst,
                               std::size_t N) noexcept
{
    for (std::size_t i = 0; i < N; i++) {
        if (src[i] > dst[i]) dst[i] = src[i];
    }
}

#if SIGNALSCOPE_HAS_AVX2
inline void max_inplace_avx2(const float* src, float* dst,
                             std::size_t N) noexcept
{
    const std::size_t lanes = 8;
    const std::size_t n_full = N & ~(lanes - 1);
    for (std::size_t i = 0; i < n_full; i += lanes) {
        __m256 a = _mm256_loadu_ps(src + i);
        __m256 b = _mm256_loadu_ps(dst + i);
        _mm256_storeu_ps(dst + i, _mm256_max_ps(a, b));
    }
    for (std::size_t i = n_full; i < N; i++) {
        if (src[i] > dst[i]) dst[i] = src[i];
    }
}
#endif // SIGNALSCOPE_HAS_AVX2

#if SIGNALSCOPE_HAS_NEON
inline void max_inplace_neon(const float* src, float* dst,
                             std::size_t N) noexcept
{
    const std::size_t lanes = 4;
    const std::size_t n_full = N & ~(lanes - 1);
    for (std::size_t i = 0; i < n_full; i += lanes) {
        float32x4_t a = vld1q_f32(src + i);
        float32x4_t b = vld1q_f32(dst + i);
        vst1q_f32(dst + i, vmaxq_f32(a, b));
    }
    for (std::size_t i = n_full; i < N; i++) {
        if (src[i] > dst[i]) dst[i] = src[i];
    }
}
#endif // SIGNALSCOPE_HAS_NEON

inline void max_inplace(const float* src, float* dst, std::size_t N) noexcept {
#if SIGNALSCOPE_HAS_AVX2
    max_inplace_avx2(src, dst, N);
#elif SIGNALSCOPE_HAS_NEON
    max_inplace_neon(src, dst, N);
#else
    max_inplace_scalar(src, dst, N);
#endif
}

} // namespace signalscope::simd
