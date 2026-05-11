#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace signalscope {

enum class Weighting {
    None,
    Flatten,    // backend leaves dB values untouched (no curve applied);
                // renderer treats this as "y-axis auto-range" using the
                // spec_min/spec_max fields the backend ships in the
                // SpectrumStats frame. Choosing it doesn't change peak
                // detection or noise-floor measurements.
    AWeight,    // IEC 61672 A-weighting (psychoacoustic, low-SPL)
    CWeight,    // IEC 61672 C-weighting (low/high cut, high-SPL)
};

inline Weighting weighting_from_string(const std::string& s) {
    if (s == "A-weight" || s == "a-weight" || s == "a") return Weighting::AWeight;
    if (s == "C-weight" || s == "c-weight" || s == "c") return Weighting::CWeight;
    if (s == "flatten") return Weighting::Flatten;
    return Weighting::None;
}

namespace detail {

// IEC 61672 A-weighting curve in dB. Even in f, so two-sided spectra work.
// The formula goes to −∞ at f = 0; clamp the magnitude to a finite floor so
// the DC bin doesn't blow up the renderer's color mapping.
inline float a_weight_db(double f) {
    const double f2 = f * f;
    const double f4 = f2 * f2;
    constexpr double k = 12194.0 * 12194.0;
    const double num = k * f4;
    const double d1 = f2 + 20.6 * 20.6;
    const double d2 = std::sqrt((f2 + 107.7 * 107.7) * (f2 + 737.9 * 737.9));
    const double d3 = f2 + k;
    const double den = d1 * d2 * d3;
    if (den <= 0.0 || num <= 0.0) return -200.0f;
    const double r = num / den;
    const double db = 20.0 * std::log10(r) + 2.00; // +2 dB so A(1 kHz) ≈ 0
    return std::max(-200.0f, static_cast<float>(db));
}

inline float c_weight_db(double f) {
    const double f2 = f * f;
    constexpr double k = 12194.0 * 12194.0;
    const double num = k * f2;
    const double d1 = f2 + 20.6 * 20.6;
    const double d3 = f2 + k;
    const double den = d1 * d3;
    if (den <= 0.0 || num <= 0.0) return -200.0f;
    const double r = num / den;
    const double db = 20.0 * std::log10(r) + 0.06; // +0.06 dB so C(1 kHz) ≈ 0
    return std::max(-200.0f, static_cast<float>(db));
}

} // namespace detail

/// Bin -> frequency (Hz) for a one-sided spectrum of length N covering
/// [0, fs/2]. Bin i represents the bin centered at i * fs / fft_size, where
/// fft_size = 2*N for a real FFT.
inline double one_sided_freq(std::size_t i, std::size_t N, double fs) {
    return static_cast<double>(i) * (fs * 0.5) / static_cast<double>(N);
}

/// Bin -> frequency (Hz) for a two-sided fftshifted spectrum of length N
/// running from -fs/2 (i=0) to +fs/2 - bin (i=N-1).
inline double two_sided_freq(std::size_t i, std::size_t N, double fs) {
    const double bin_hz = fs / static_cast<double>(N);
    return (static_cast<double>(i) - static_cast<double>(N) / 2.0) * bin_hz;
}

/// Apply a stateless weighting curve in-place. A/C weighting is an additive
/// per-bin offset; None and Flatten are no-ops on the dB values themselves.
/// Flatten still triggers renderer-side y-axis auto-range (driven by the
/// spec_min/spec_max fields shipped in SpectrumStats), but no per-bin
/// transformation happens here.
inline void apply_weighting(std::span<float> spec,
                            Weighting w, double fs, bool two_sided) noexcept
{
    if (spec.empty() || w == Weighting::None) return;
    if (w == Weighting::Flatten) return;

    auto curve = (w == Weighting::AWeight) ? detail::a_weight_db
                                           : detail::c_weight_db;
    const std::size_t N = spec.size();
    for (std::size_t i = 0; i < N; i++) {
        const double f = two_sided ? two_sided_freq(i, N, fs)
                                   : one_sided_freq(i, N, fs);
        // Curve is even in f; std::abs handles the negative half of a
        // two-sided spectrum.
        spec[i] += curve(std::abs(f));
    }
}

} // namespace signalscope
