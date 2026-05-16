#pragma once
#include "dsp/simd_ops/common.hpp"

namespace signalscope::simd {

// out[i] = in[i] * window[i] * gain. Sized loop; tail handled by scalar.
inline void window_apply_scalar(const float* in, const float* window,
                                float* out, std::size_t N, float gain) noexcept
{
    for (std::size_t i = 0; i < N; i++) {
        out[i] = in[i] * window[i] * gain;
    }
}

#if SIGNALSCOPE_HAS_AVX2
inline void window_apply_avx2(const float* in, const float* window,
                              float* out, std::size_t N, float gain) noexcept
{
    const __m256 vgain = _mm256_set1_ps(gain);
    const std::size_t lanes = 8;
    const std::size_t n_full = N & ~(lanes - 1);
    for (std::size_t i = 0; i < n_full; i += lanes) {
        __m256 a = _mm256_loadu_ps(in + i);
        __m256 w = _mm256_loadu_ps(window + i);
        __m256 r = _mm256_mul_ps(_mm256_mul_ps(a, w), vgain);
        _mm256_storeu_ps(out + i, r);
    }
    for (std::size_t i = n_full; i < N; i++) {
        out[i] = in[i] * window[i] * gain;
    }
}
#endif // SIGNALSCOPE_HAS_AVX2

#if SIGNALSCOPE_HAS_NEON
inline void window_apply_neon(const float* in, const float* window,
                              float* out, std::size_t N, float gain) noexcept
{
    const float32x4_t vgain = vdupq_n_f32(gain);
    const std::size_t lanes = 4;
    const std::size_t n_full = N & ~(lanes - 1);
    for (std::size_t i = 0; i < n_full; i += lanes) {
        float32x4_t a = vld1q_f32(in + i);
        float32x4_t w = vld1q_f32(window + i);
        float32x4_t r = vmulq_f32(vmulq_f32(a, w), vgain);
        vst1q_f32(out + i, r);
    }
    for (std::size_t i = n_full; i < N; i++) {
        out[i] = in[i] * window[i] * gain;
    }
}
#endif // SIGNALSCOPE_HAS_NEON

inline void window_apply(const float* in, const float* window,
                         float* out, std::size_t N, float gain) noexcept {
#if SIGNALSCOPE_HAS_AVX2
    window_apply_avx2(in, window, out, N, gain);
#elif SIGNALSCOPE_HAS_NEON
    window_apply_neon(in, window, out, N, gain);
#else
    window_apply_scalar(in, window, out, N, gain);
#endif
}

} // namespace signalscope::simd
