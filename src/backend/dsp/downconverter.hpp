#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>
#include <vector>

namespace signalscope {

// Digital down-converter:
//   input (real or complex) -> mix to baseband at -tune_hz -> low-pass FIR
//   -> decimate by M -> complex output at fs/M
//
// When tune_hz==0 and M==1 the stage is a pass-through (active() returns
// false). The DSP loop in main.cpp checks active() before routing samples
// through this class, so the steady-state cost when the user hasn't touched
// the Tune/Decimate knobs is one atomic load.
//
// FIR design: 65-tap windowed-sinc with Hamming window. Cutoff is fs/(2M),
// so post-decimation Nyquist is the new band edge. Taps are recomputed on
// configure(). Filter state (delay line) survives across blocks so back-to-
// back calls splice cleanly.
class DownConverter {
public:
    void configure(float tune_hz, std::size_t decimation, float source_sr) {
        tune_hz_     = tune_hz;
        decimation_  = decimation < 1 ? 1 : decimation;
        source_sr_   = source_sr;
        // Negative phase increment: shifts the band centred at +tune_hz
        // down to 0 Hz when multiplied by exp(j*phase).
        phase_inc_   = -2.0 * std::numbers::pi * static_cast<double>(tune_hz)
                                               / static_cast<double>(source_sr);
        rebuild_filter();
        ensure_history();
    }

    bool   active() const { return tune_hz_ != 0.0f || decimation_ > 1; }
    float  effective_sample_rate() const {
        return source_sr_ / static_cast<float>(decimation_);
    }
    std::size_t decimation() const { return decimation_; }
    float       tune_hz()    const { return tune_hz_; }

    // Mix REAL input -> complex baseband, low-pass, decimate. input length
    // must be a multiple of decimation_; output length = input.size()/decimation_.
    void process_real(std::span<const float> in,
                      std::vector<float>& out_re,
                      std::vector<float>& out_im)
    {
        const std::size_t input_len = in.size();
        const std::size_t out_len = input_len / decimation_;
        out_re.resize(out_len);
        out_im.resize(out_len);
        ensure_history();

        std::size_t out_idx = 0;
        std::size_t mod = 0;
        const std::size_t L = fir_taps_.size();
        for (std::size_t i = 0; i < input_len; i++) {
            const float c = std::cos(phase_);
            const float s = std::sin(phase_);
            const float x = in[i];
            history_re_[hist_idx_] = x * c;
            history_im_[hist_idx_] = x * s;
            hist_idx_ = (hist_idx_ + 1) % L;
            advance_phase();

            if (++mod == decimation_) {
                mod = 0;
                float ar = 0, ai = 0;
                std::size_t idx = hist_idx_;
                for (std::size_t k = 0; k < L; k++) {
                    ar += fir_taps_[k] * history_re_[idx];
                    ai += fir_taps_[k] * history_im_[idx];
                    idx = (idx + 1) % L;
                }
                out_re[out_idx]   = ar;
                out_im[out_idx++] = ai;
            }
        }
    }

    // Mix COMPLEX input - used when the source is already IQ (file/SDR) and
    // the user is re-tuning within the captured band. in_re and in_im must
    // be the same length; only the shorter is read if they differ.
    void process_complex(std::span<const float> in_re,
                         std::span<const float> in_im,
                         std::vector<float>& out_re,
                         std::vector<float>& out_im)
    {
        const std::size_t input_len = std::min(in_re.size(), in_im.size());
        const std::size_t out_len = input_len / decimation_;
        out_re.resize(out_len);
        out_im.resize(out_len);
        ensure_history();

        std::size_t out_idx = 0;
        std::size_t mod = 0;
        const std::size_t L = fir_taps_.size();
        for (std::size_t i = 0; i < input_len; i++) {
            const float c = std::cos(phase_);
            const float s = std::sin(phase_);
            const float a = in_re[i];
            const float b = in_im[i];
            // (a + jb) * (c + js) = (ac − bs) + j(as + bc)
            history_re_[hist_idx_] = a * c - b * s;
            history_im_[hist_idx_] = a * s + b * c;
            hist_idx_ = (hist_idx_ + 1) % L;
            advance_phase();

            if (++mod == decimation_) {
                mod = 0;
                float ar = 0, ai = 0;
                std::size_t idx = hist_idx_;
                for (std::size_t k = 0; k < L; k++) {
                    ar += fir_taps_[k] * history_re_[idx];
                    ai += fir_taps_[k] * history_im_[idx];
                    idx = (idx + 1) % L;
                }
                out_re[out_idx]   = ar;
                out_im[out_idx++] = ai;
            }
        }
    }

    void reset() {
        phase_ = 0;
        std::fill(history_re_.begin(), history_re_.end(), 0.0f);
        std::fill(history_im_.begin(), history_im_.end(), 0.0f);
        hist_idx_ = 0;
    }

private:
    static constexpr std::size_t FIR_LEN = 65;

    float       tune_hz_     = 0;
    std::size_t decimation_  = 1;
    float       source_sr_   = 48000;
    double      phase_       = 0;
    double      phase_inc_   = 0;
    std::vector<float> fir_taps_;
    std::vector<float> history_re_;
    std::vector<float> history_im_;
    std::size_t hist_idx_ = 0;

    void advance_phase() {
        phase_ += phase_inc_;
        constexpr double pi  = std::numbers::pi;
        constexpr double tau = 2.0 * pi;
        if      (phase_ >  pi) phase_ -= tau;
        else if (phase_ < -pi) phase_ += tau;
    }

    void ensure_history() {
        if (history_re_.size() != FIR_LEN) {
            history_re_.assign(FIR_LEN, 0.0f);
            history_im_.assign(FIR_LEN, 0.0f);
            hist_idx_ = 0;
        }
    }

    void rebuild_filter() {
        fir_taps_.resize(FIR_LEN);
        // Cutoff in cycles/sample. After decimating by M the new Nyquist
        // is fs/(2M), so the normalized cutoff is 1/(2M).
        const double fc = 1.0 / (2.0 * static_cast<double>(decimation_));
        const int N = static_cast<int>(FIR_LEN);
        const double mid = (N - 1) / 2.0;
        constexpr double pi = std::numbers::pi;
        double sum = 0;
        for (int n = 0; n < N; n++) {
            const double k = n - mid;
            double tap;
            if (k == 0.0) tap = 2.0 * fc;
            else          tap = std::sin(2.0 * pi * fc * k) / (pi * k);
            const double w = 0.54 - 0.46 *
                std::cos(2.0 * pi * static_cast<double>(n) / (N - 1));
            tap *= w;
            fir_taps_[n] = static_cast<float>(tap);
            sum += tap;
        }
        if (sum > 0.0) {
            const float inv = 1.0f / static_cast<float>(sum);
            for (auto& t : fir_taps_) t *= inv;
        }
    }
};

} // namespace signalscope
