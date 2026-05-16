#pragma once
#include "dsp/simd_ops/common.hpp"

namespace signalscope::simd {

// out[i] = (mag_i > 1e-10) ? 20*log10(mag_i) : -200
// where mag_i = sqrt((re_im[2i])^2 + (re_im[2i+1])^2) * norm
//
// re_im is a planar pair: 2*N floats laid out as r0, i0, r1, i1, ...
// (matching std::complex<float>'s memory layout). out has N floats.
inline void magnitude_db_scalar(const float* re_im, float* out,
                                std::size_t halfN, float norm) noexcept
{
    constexpr float kFloor = -200.0f;
    constexpr float kEps   = 1e-10f;
    for (std::size_t i = 0; i < halfN; i++) {
        const float re = re_im[2*i + 0] * norm;
        const float im = re_im[2*i + 1] * norm;
        const float mag = std::sqrt(re * re + im * im);
        out[i] = (mag > kEps) ? 20.0f * std::log10(mag) : kFloor;
    }
}

#if SIGNALSCOPE_HAS_AVX2
// 10·log10(x) approximation on __m256 for x > 0. Uses the Mercator
// series for ln(m) on m ∈ [1, 2): let u = (m-1)/(m+1), then
//   ln(m) = 2·(u + u³/3 + u⁵/5 + ...).
// With m clamped to [1, 2), u ∈ [0, 1/3] and three terms (degree 5)
// give |residual| ≤ (1/3)⁷ / 7 ≈ 6.5e-5 in ln, ≈ 2.8e-5 in log10,
// ≈ 5.7e-4 in 10·log10 — well under 0.001 dB. The constants in the
// final combine are math identities, not fit parameters:
//   10·log10(2)   = 3.01029995663981
//   10/ln(10)     = 4.342944819032518
static inline __m256 ten_log10_ps_avx2(__m256 x) {
    const __m256i exp_mask  = _mm256_set1_epi32(0x7F800000);
    const __m256i mant_mask = _mm256_set1_epi32(0x007FFFFF);
    const __m256i one_bits  = _mm256_castps_si256(_mm256_set1_ps(1.0f));

    const __m256i xi = _mm256_castps_si256(x);
    const __m256  e_f = _mm256_cvtepi32_ps(
        _mm256_sub_epi32(_mm256_srli_epi32(_mm256_and_si256(xi, exp_mask), 23),
                         _mm256_set1_epi32(127)));
    const __m256  m = _mm256_castsi256_ps(
        _mm256_or_si256(_mm256_and_si256(xi, mant_mask), one_bits));

    const __m256 one = _mm256_set1_ps(1.0f);
    const __m256 num = _mm256_sub_ps(m, one);
    const __m256 den = _mm256_add_ps(m, one);
    const __m256 u   = _mm256_div_ps(num, den);
    const __m256 u2  = _mm256_mul_ps(u, u);

    // ln(m) ≈ u · (2 + u²·(2/3 + u²·2/5))
    const __m256 c5 = _mm256_set1_ps(2.0f / 5.0f);
    const __m256 c3 = _mm256_set1_ps(2.0f / 3.0f);
    const __m256 c1 = _mm256_set1_ps(2.0f);
    __m256 p = _mm256_fmadd_ps(c5, u2, c3);
    p = _mm256_fmadd_ps(p, u2, c1);
    const __m256 ln_m = _mm256_mul_ps(u, p);

    const __m256 k_e = _mm256_set1_ps(3.01029995663981f);   // 10·log10(2)
    const __m256 k_m = _mm256_set1_ps(4.342944819032518f);  // 10/ln(10)
    return _mm256_fmadd_ps(e_f, k_e, _mm256_mul_ps(ln_m, k_m));
}

inline void magnitude_db_avx2(const float* re_im, float* out,
                              std::size_t halfN, float norm) noexcept
{
    constexpr float kFloor = -200.0f;
    constexpr float kEpsSq = 1e-20f;        // square of 1e-10
    const __m256 vnorm2 = _mm256_set1_ps(norm * norm);  // applied to mag²
    const __m256 veps2  = _mm256_set1_ps(kEpsSq);
    const __m256 vfloor = _mm256_set1_ps(kFloor);

    const std::size_t lanes = 8;
    const std::size_t n_full = halfN & ~(lanes - 1);
    for (std::size_t i = 0; i < n_full; i += lanes) {
        // 16 input floats = 8 (re, im) pairs.
        __m256 a = _mm256_loadu_ps(re_im + 2 * i);     // r0 i0 r1 i1 | r2 i2 r3 i3
        __m256 b = _mm256_loadu_ps(re_im + 2 * i + 8); // r4 i4 r5 i5 | r6 i6 r7 i7

        // shuffle_ps with 0x88 = picks {a[0], a[2], b[0], b[2]} per 128-bit
        // lane → [r0 r1 r4 r5 | r2 r3 r6 r7]. 0xDD picks the odd positions.
        __m256 even = _mm256_shuffle_ps(a, b, 0x88);
        __m256 odd  = _mm256_shuffle_ps(a, b, 0xDD);

        // After shuffle the 64-bit blocks within each 256-bit register are
        // ordered {0, 2, 1, 3}; permute4x64 to {0, 1, 2, 3} gives lane-correct
        // [r0 r1 r2 r3 r4 r5 r6 r7] and [i0 i1 i2 i3 i4 i5 i6 i7].
        __m256 re = _mm256_castpd_ps(
            _mm256_permute4x64_pd(_mm256_castps_pd(even), _MM_SHUFFLE(3, 1, 2, 0)));
        __m256 im = _mm256_castpd_ps(
            _mm256_permute4x64_pd(_mm256_castps_pd(odd),  _MM_SHUFFLE(3, 1, 2, 0)));

        // mag² = (re² + im²) · norm²; we compute 10·log10(mag²) directly,
        // which equals 20·log10(mag), so no sqrt is needed.
        __m256 mag2 = _mm256_fmadd_ps(re, re, _mm256_mul_ps(im, im));
        mag2 = _mm256_mul_ps(mag2, vnorm2);

        __m256 mask = _mm256_cmp_ps(mag2, veps2, _CMP_GT_OS);
        // Substitute 1.0 where masked off so the log approximation never
        // sees a denormal/zero input; blend the floor back in afterward.
        __m256 safe = _mm256_blendv_ps(_mm256_set1_ps(1.0f), mag2, mask);
        __m256 lg   = ten_log10_ps_avx2(safe);
        __m256 result = _mm256_blendv_ps(vfloor, lg, mask);
        _mm256_storeu_ps(out + i, result);
    }
    // Tail uses the scalar 20·log10(mag) for byte-identical edge behavior.
    for (std::size_t i = n_full; i < halfN; i++) {
        const float re = re_im[2*i + 0] * norm;
        const float im = re_im[2*i + 1] * norm;
        const float mag = std::sqrt(re * re + im * im);
        out[i] = (mag > 1e-10f) ? 20.0f * std::log10(mag) : kFloor;
    }
}
#endif // SIGNALSCOPE_HAS_AVX2

#if SIGNALSCOPE_HAS_NEON
// 10·log10(x) approximation on float32x4_t. Same Mercator-series
// approach as the AVX2 path; see comments there for the error bound.
static inline float32x4_t ten_log10_ps_neon(float32x4_t x) {
    const int32x4_t   exp_mask  = vdupq_n_s32(0x7F800000);
    const int32x4_t   mant_mask = vdupq_n_s32(0x007FFFFF);
    const int32x4_t   one_bits  = vreinterpretq_s32_f32(vdupq_n_f32(1.0f));

    const int32x4_t   xi    = vreinterpretq_s32_f32(x);
    const int32x4_t   e_raw = vshrq_n_s32(vandq_s32(xi, exp_mask), 23);
    const float32x4_t e_f   = vcvtq_f32_s32(vsubq_s32(e_raw, vdupq_n_s32(127)));
    const float32x4_t m     = vreinterpretq_f32_s32(
        vorrq_s32(vandq_s32(xi, mant_mask), one_bits));

    const float32x4_t one = vdupq_n_f32(1.0f);
    const float32x4_t num = vsubq_f32(m, one);
    const float32x4_t den = vaddq_f32(m, one);
    // NEON has no full reciprocal in basic ISA; vdivq_f32 is aarch64 only.
    // For portability use the reciprocal estimate + two Newton refinement
    // steps, which saturates at FP32 precision (~24 effective bits). One
    // step (~22 bits) is too few for the Mercator bound at m near 2.
    float32x4_t rcp = vrecpeq_f32(den);
    rcp = vmulq_f32(rcp, vrecpsq_f32(den, rcp));
    rcp = vmulq_f32(rcp, vrecpsq_f32(den, rcp));
    const float32x4_t u  = vmulq_f32(num, rcp);
    const float32x4_t u2 = vmulq_f32(u, u);

    const float32x4_t c5 = vdupq_n_f32(2.0f / 5.0f);
    const float32x4_t c3 = vdupq_n_f32(2.0f / 3.0f);
    const float32x4_t c1 = vdupq_n_f32(2.0f);
    // vmlaq_f32(a, b, c) = a + b*c
    float32x4_t p = vmlaq_f32(c3, c5, u2);
    p = vmlaq_f32(c1, p, u2);
    const float32x4_t ln_m = vmulq_f32(u, p);

    const float32x4_t k_e = vdupq_n_f32(3.01029995663981f);
    const float32x4_t k_m = vdupq_n_f32(4.342944819032518f);
    return vmlaq_f32(vmulq_f32(ln_m, k_m), e_f, k_e);
}

inline void magnitude_db_neon(const float* re_im, float* out,
                              std::size_t halfN, float norm) noexcept
{
    constexpr float kFloor = -200.0f;
    constexpr float kEpsSq = 1e-20f;
    const float32x4_t vnorm2 = vdupq_n_f32(norm * norm);
    const float32x4_t veps2  = vdupq_n_f32(kEpsSq);
    const float32x4_t vfloor = vdupq_n_f32(kFloor);
    const float32x4_t vone   = vdupq_n_f32(1.0f);

    const std::size_t lanes = 4;
    const std::size_t n_full = halfN & ~(lanes - 1);
    for (std::size_t i = 0; i < n_full; i += lanes) {
        float32x4x2_t deint = vld2q_f32(re_im + 2 * i);
        float32x4_t re = deint.val[0];
        float32x4_t im = deint.val[1];
        float32x4_t mag2 = vmlaq_f32(vmulq_f32(re, re), im, im);
        mag2 = vmulq_f32(mag2, vnorm2);

        uint32x4_t mask = vcgtq_f32(mag2, veps2);
        // Substitute 1.0 where masked off so the log never sees zero.
        float32x4_t safe = vbslq_f32(mask, mag2, vone);
        float32x4_t lg   = ten_log10_ps_neon(safe);
        float32x4_t result = vbslq_f32(mask, lg, vfloor);
        vst1q_f32(out + i, result);
    }
    for (std::size_t i = n_full; i < halfN; i++) {
        const float re = re_im[2*i + 0] * norm;
        const float im = re_im[2*i + 1] * norm;
        const float mag = std::sqrt(re * re + im * im);
        out[i] = (mag > 1e-10f) ? 20.0f * std::log10(mag) : kFloor;
    }
}
#endif // SIGNALSCOPE_HAS_NEON

inline void magnitude_db(const float* re_im, float* out,
                         std::size_t halfN, float norm) noexcept {
#if SIGNALSCOPE_HAS_AVX2
    magnitude_db_avx2(re_im, out, halfN, norm);
#elif SIGNALSCOPE_HAS_NEON
    magnitude_db_neon(re_im, out, halfN, norm);
#else
    magnitude_db_scalar(re_im, out, halfN, norm);
#endif
}

} // namespace signalscope::simd
