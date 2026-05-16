#pragma once
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>
#include <algorithm>
#include <numbers>
#include <numeric>
#include <complex>
#include <limits>
#include <span>
#include <string>
#include "protocol/protocol.hpp"
#include "dsp/fft/fft_backend.hpp"
#include "dsp/simd_ops.hpp"

namespace signalscope {
    struct Config {
        size_t fft_size     = 4096;
        float  sample_rate  = 48000.0f;
        WindowFunction window = WindowFunction::Hanning;
        float  gain_db      = 0.0f;
        size_t avg_count    = 1;
    };

class DSPPipeline {
public:

    explicit DSPPipeline(Config cfg = {})
        : config_(cfg), fft_(ss::fft::make_backend())
    {
        resize(cfg.fft_size);
    }

    ~DSPPipeline() = default;

    void set_fft_size(size_t n) {
        config_.fft_size = n;
        resize(n);
    }

    void set_sample_rate(float sr) { config_.sample_rate = sr; }
    void set_window(WindowFunction w) { config_.window = w; compute_window(); }
    void set_window(const std::string& name) { set_window(window_from_string(name)); }
    void set_gain(float db) { config_.gain_db = db; }
    void set_avg_count(size_t n) { config_.avg_count = std::max<size_t>(1, n); }
    const Config& config() const { return config_; }

    // FFT window-stride overlap. factor=1 means back-to-back windows
    // (hop = fft_size); factor=N keeps (N-1)/N of the previous window
    // and appends fft_size/N fresh samples. Supported set {1,2,4,8,16}.
    // Callers pass hop_size() samples per call rather than fft_size; the
    // slide buffer lives inside this class.
    void set_overlap(int factor) {
        if (factor < 1) factor = 1;
        // Quantize to the supported power-of-two set; non-powers would
        // give a non-integer hop.
        if      (factor >= 16) factor = 16;
        else if (factor >= 8)  factor = 8;
        else if (factor >= 4)  factor = 4;
        else if (factor >= 2)  factor = 2;
        else                   factor = 1;
        if (overlap_factor_ == factor) return;
        overlap_factor_ = factor;
        // Clear slide buffers — one window of warm-up beats emitting
        // a frame whose front half was windowed by the old config.
        std::fill(slide_real_.begin(), slide_real_.end(), 0.0f);
        std::fill(slide_re_.begin(),   slide_re_.end(),   0.0f);
        std::fill(slide_im_.begin(),   slide_im_.end(),   0.0f);
    }
    int    overlap_factor() const { return overlap_factor_; }
    size_t hop_size()       const { return config_.fft_size / overlap_factor_; }

    // Window calibration constants - exposed for tests and any caller
    // that wants PSD-mode normalization.
    float coherent_gain() const { return coherent_gain_; }
    float enbw_bins()     const { return enbw_bins_; }

    // Multitaper estimator
    // Multitaper estimator: K=1 keeps the configured window; K>1
    // averages K orthogonal sine-taper (Riedel-Sidorenko 1995) FFTs
    // per frame for ~1/K variance reduction at K× FFT cost. Tapers
    // each have a different DC response, so mt_cg_ caches the RMS
    // coherent gain and divides the norm to keep tone amplitude
    // consistent with the single-window path.
    void set_taper_count(int k) {
        if (k < 1) k = 1;
        // Round to nearest power of two in {1, 2, 4, 8} so each taper
        // gets a clean orthogonal frequency band.
        if      (k >= 8) k = 8;
        else if (k >= 4) k = 4;
        else if (k >= 2) k = 2;
        else             k = 1;
        if (taper_count_ == k) return;
        taper_count_ = k;
        compute_tapers();
    }
    int taper_count() const { return taper_count_; }

    // Per-bin running max across frames since the last clear; lets
    // bursty signals stay visible after they decay.
    void set_peak_hold(bool on) {
        if (on && !peak_hold_enabled_) {
            // Seed from the next frame so we don't display a wall of -inf.
            peak_hold_seeded_ = false;
            peak_hold_full_seeded_ = false;
        }
        peak_hold_enabled_ = on;
    }
    void clear_peak_hold() {
        peak_hold_seeded_ = false;
        peak_hold_full_seeded_ = false;
    }
    bool peak_hold_enabled() const { return peak_hold_enabled_; }

    /// Real input -> one-sided dBFS magnitude (size = fft_size/2).
    /// overlap=1: pass at least `fft_size` samples.
    /// overlap>1: pass at most `hop_size()` per call; the slide buffer
    ///            here holds the retained prefix from the prior call.
    const std::vector<float>& process_spectrum(std::span<const float> input) {
        const size_t N = config_.fft_size;
        const size_t halfN = N / 2;
        const size_t input_len = input.size();
        const float gain_linear = std::pow(10.0f, config_.gain_db / 20.0f);

        // Backend-owned input/output buffers. Stable until resize().
        float*                     in_real = fft_->input_real();
        const std::complex<float>* out_r2c = fft_->output_r2c();

        // Resolve the FFT input window. The slide path is only entered when
        // overlap is engaged; the no-overlap fast path stays bit-identical
        // to its old behavior.
        std::size_t scan_len;       // how much of `input` is "fresh" (clip-tested)
        if (overlap_factor_ == 1) {
            const size_t apply_len = std::min(N, input_len);
            simd::window_apply(input.data(), window_coeffs_.data(),
                               in_real, apply_len, gain_linear);
            for (size_t i = apply_len; i < N; i++)
                in_real[i] = 0.0f;
            scan_len = apply_len;
        } else {
            // Slide left by hop, append new samples to the tail.
            const size_t hop = N / overlap_factor_;
            const size_t keep = N - hop;
            std::copy(slide_real_.begin() + hop, slide_real_.end(),
                      slide_real_.begin());
            const size_t fresh = std::min(hop, input_len);
            for (size_t i = 0; i < fresh; i++) slide_real_[keep + i] = input[i];
            for (size_t i = fresh; i < hop;  i++) slide_real_[keep + i] = 0.0f;
            simd::window_apply(slide_real_.data(), window_coeffs_.data(),
                               in_real, N, gain_linear);
            scan_len = fresh;
        }

        clipping_ = false;
        for (size_t i = 0; i < scan_len; i++) {
            if (std::abs(input[i]) >= 0.999f) { clipping_ = true; break; }
        }

        if (taper_count_ > 1) {
            // Multitaper path: the FFT input window is whatever the
            // single-window path JUST wrote into in_real, but
            // pre-window-multiplication. Reconstruct it (pre-window) by
            // dividing out window_coeffs_, then loop K times applying
            // each taper. Cheaper alternative: snapshot the source data
            // before windowing. Use the slide buffer when overlap > 1
            // (already populated above); for overlap == 1 read from
            // `input` directly.
            const float* raw = (overlap_factor_ == 1) ? input.data()
                                                       : slide_real_.data();
            const size_t raw_n = (overlap_factor_ == 1)
                ? std::min(N, input_len)
                : N;  // slide buffer is always full-N
            std::fill(psd_acc_.begin(), psd_acc_.end(), 0.0f);
            for (int k = 0; k < taper_count_; k++) {
                const float* taper = tapers_[k].data();
                for (size_t i = 0; i < raw_n; i++)
                    in_real[i] = raw[i] * taper[i] * gain_linear;
                for (size_t i = raw_n; i < N; i++)
                    in_real[i] = 0.0f;
                fft_->execute_r2c();
                for (size_t i = 0; i < halfN; i++) {
                    const float re = out_r2c[i].real();
                    const float im = out_r2c[i].imag();
                    psd_acc_[i] += re * re + im * im;
                }
            }
            const float Kf   = static_cast<float>(taper_count_);
            const float norm = 2.0f / (static_cast<float>(N) * mt_cg_);
            for (size_t i = 0; i < halfN; i++) {
                const float mean_psd = psd_acc_[i] / Kf;
                const float mag = std::sqrt(mean_psd) * norm;
                magnitude_[i] = (mag > 1e-10f) ? 20.0f * std::log10(mag) : -200.0f;
            }
        } else {
            fft_->execute_r2c();

            // 2/N is the standard real-FFT normalization (factor 2 because
            // conjugate-symmetric pairs combine; 1/N is plain DFT scaling).
            // Dividing by coherent_gain_ compensates for window attenuation -
            // makes a unit-amplitude tone read 0 dBFS regardless of window.
            const float norm = 2.0f / (static_cast<float>(N) * coherent_gain_);
            // out_r2c is std::complex<float>* — same memory layout as
            // an interleaved float pair, so the cast is safe.
            simd::magnitude_db(reinterpret_cast<const float*>(out_r2c),
                               magnitude_.data(), halfN, norm);
        }

        if (config_.avg_count > 1) {
            avg_history_.push_back(magnitude_);
            if (avg_history_.size() > config_.avg_count) {
                avg_history_.erase(avg_history_.begin());
            }
            for (size_t i = 0; i < halfN; i++) {
                float sum = 0.0f;
                for (const auto& hist : avg_history_) sum += hist[i];
                magnitude_[i] = sum / static_cast<float>(avg_history_.size());
            }
        }

        // Update the peak-hold side buffer as a separate trace. We intentionally
        // do NOT overwrite magnitude_ - the live trace is what the spectrogram
        // waterfall (and any other consumer) needs. main.cpp emits the held
        // buffer as its own frame type when peak_hold_enabled_.
        if (peak_hold_enabled_) {
            if (!peak_hold_seeded_ || peak_hold_.size() != halfN) {
                peak_hold_.assign(magnitude_.begin(), magnitude_.end());
                peak_hold_seeded_ = true;
            } else {
                simd::max_inplace(magnitude_.data(), peak_hold_.data(), halfN);
            }
        }

        return magnitude_;
    }

    /// Peak-hold trace from the most recent process_spectrum() call (size =
    /// fft_size/2). Only meaningful when peak_hold_enabled() is true.
    const std::vector<float>& peak_hold_trace() const { return peak_hold_; }

    /// One-sided phase output (size = fft_size/2), produced by the most recent
    /// real FFT call.
    const std::vector<float>& get_phase() {
        const size_t halfN = config_.fft_size / 2;
        phase_.resize(halfN);
        const std::complex<float>* out_r2c = fft_->output_r2c();
        for (size_t i = 0; i < halfN; i++) {
            phase_[i] = std::atan2(out_r2c[i].imag(), out_r2c[i].real());
        }
        return phase_;
    }

    // Complex-input processing (two-sided spectrum, N bins)
    /// Process a block of complex samples.
    ///
    /// With overlap = 1 (default), pass at least `fft_size` entries. With
    /// overlap > 1, pass at most `hop_size()` entries - the pipeline
    /// slides the internal complex buffer and appends the new samples
    /// before FFTing.
    ///
    /// Returns a two-sided magnitude spectrum in dBFS, size = fft_size,
    /// ordered from -fs/2 to +fs/2 (fft-shifted). The two spans must
    /// have the same length; only the shorter is used if they differ.
    const std::vector<float>& process_spectrum_complex(
        std::span<const float> re, std::span<const float> im)
    {
        const size_t N = config_.fft_size;
        const size_t input_len = std::min(re.size(), im.size());
        const float gain_linear = std::pow(10.0f, config_.gain_db / 20.0f);

        // Backend-owned input/output buffers. Stable until resize().
        std::complex<float>*       in_c2c  = fft_->input_complex();
        const std::complex<float>* out_c2c = fft_->output_complex();

        std::size_t scan_len;
        if (overlap_factor_ == 1) {
            for (size_t i = 0; i < N && i < input_len; i++) {
                const float w = window_coeffs_[i] * gain_linear;
                in_c2c[i].real(re[i] * w);
                in_c2c[i].imag(im[i] * w);
            }
            for (size_t i = input_len; i < N; i++) {
                in_c2c[i] = {};
            }
            scan_len = std::min(N, input_len);
        } else {
            // Slide both re/im buffers left by hop, append new samples,
            // window+gain into the FFT input.
            const size_t hop  = N / overlap_factor_;
            const size_t keep = N - hop;
            std::copy(slide_re_.begin() + hop, slide_re_.end(), slide_re_.begin());
            std::copy(slide_im_.begin() + hop, slide_im_.end(), slide_im_.begin());
            const size_t fresh = std::min(hop, input_len);
            for (size_t i = 0; i < fresh; i++) {
                slide_re_[keep + i] = re[i];
                slide_im_[keep + i] = im[i];
            }
            for (size_t i = fresh; i < hop; i++) {
                slide_re_[keep + i] = 0.0f;
                slide_im_[keep + i] = 0.0f;
            }
            for (size_t i = 0; i < N; i++) {
                const float w = window_coeffs_[i] * gain_linear;
                in_c2c[i].real(slide_re_[i] * w);
                in_c2c[i].imag(slide_im_[i] * w);
            }
            scan_len = fresh;
        }

        clipping_ = false;
        for (size_t i = 0; i < scan_len; i++) {
            const float mag = std::sqrt(re[i]*re[i] + im[i]*im[i]);
            if (mag >= 0.999f) { clipping_ = true; break; }
        }

        if (taper_count_ > 1) {
            // Multitaper for the complex path mirrors the real path -
            // K FFTs with sine tapers, average squared magnitudes,
            // single dB conversion at the end. The two-sided fftshift
            // happens during the conversion step.
            const float* raw_re = (overlap_factor_ == 1) ? re.data()
                                                          : slide_re_.data();
            const float* raw_im = (overlap_factor_ == 1) ? im.data()
                                                          : slide_im_.data();
            const size_t raw_n  = (overlap_factor_ == 1)
                ? std::min(N, input_len) : N;
            std::fill(psd_acc_full_.begin(), psd_acc_full_.end(), 0.0f);
            for (int k = 0; k < taper_count_; k++) {
                const float* taper = tapers_[k].data();
                for (size_t i = 0; i < raw_n; i++) {
                    const float w = taper[i] * gain_linear;
                    in_c2c[i].real(raw_re[i] * w);
                    in_c2c[i].imag(raw_im[i] * w);
                }
                for (size_t i = raw_n; i < N; i++) {
                    in_c2c[i] = {};
                }
                fft_->execute_c2c_forward();
                const size_t half = N / 2;
                for (size_t i = 0; i < N; i++) {
                    const size_t src = (i + half) % N;
                    const float r2 = out_c2c[src].real();
                    const float i2 = out_c2c[src].imag();
                    psd_acc_full_[i] += r2 * r2 + i2 * i2;
                }
            }
            magnitude_full_.resize(N);
            const float Kf   = static_cast<float>(taper_count_);
            const float norm = 1.0f / (static_cast<float>(N) * mt_cg_);
            for (size_t i = 0; i < N; i++) {
                const float mean_psd = psd_acc_full_[i] / Kf;
                const float mag = std::sqrt(mean_psd) * norm;
                magnitude_full_[i] = (mag > 1e-10f) ? 20.0f * std::log10(mag) : -200.0f;
            }
            // Skip the rest of the single-window pipeline below.
            if (config_.avg_count > 1) {
                avg_history_full_.push_back(magnitude_full_);
                if (avg_history_full_.size() > config_.avg_count) {
                    avg_history_full_.erase(avg_history_full_.begin());
                }
                for (size_t i = 0; i < N; i++) {
                    float sum = 0.0f;
                    for (const auto& hist : avg_history_full_) sum += hist[i];
                    magnitude_full_[i] = sum / static_cast<float>(avg_history_full_.size());
                }
            }
            if (peak_hold_enabled_) {
                if (!peak_hold_full_seeded_ || peak_hold_full_.size() != N) {
                    peak_hold_full_.assign(magnitude_full_.begin(), magnitude_full_.end());
                    peak_hold_full_seeded_ = true;
                } else {
                    simd::max_inplace(magnitude_full_.data(),
                                      peak_hold_full_.data(), N);
                }
            }
            return magnitude_full_;
        }

        fft_->execute_c2c_forward();

        magnitude_full_.resize(N);
        // 1/N is the standard DFT normalization for complex input (no
        // factor of 2 - every bin is independent). Coherent-gain divisor
        // matches the real-input path so windowing doesn't shift dB.
        const float norm = 1.0f / (static_cast<float>(N) * coherent_gain_);
        // fftshift: source index `(i + N/2) % N` -> output index i, so the
        // output runs from -fs/2 (i=0) to +fs/2-bin (i=N-1).
        const size_t half = N / 2;
        for (size_t i = 0; i < N; i++) {
            const size_t src = (i + half) % N;
            const float r  = out_c2c[src].real() * norm;
            const float ix = out_c2c[src].imag() * norm;
            const float mag = std::sqrt(r * r + ix * ix);
            magnitude_full_[i] = (mag > 1e-10f) ? 20.0f * std::log10(mag) : -200.0f;
        }

        if (config_.avg_count > 1) {
            avg_history_full_.push_back(magnitude_full_);
            if (avg_history_full_.size() > config_.avg_count) {
                avg_history_full_.erase(avg_history_full_.begin());
            }
            for (size_t i = 0; i < N; i++) {
                float sum = 0.0f;
                for (const auto& hist : avg_history_full_) sum += hist[i];
                magnitude_full_[i] = sum / static_cast<float>(avg_history_full_.size());
            }
        }

        if (peak_hold_enabled_) {
            if (!peak_hold_full_seeded_ || peak_hold_full_.size() != N) {
                peak_hold_full_.assign(magnitude_full_.begin(), magnitude_full_.end());
                peak_hold_full_seeded_ = true;
            } else {
                simd::max_inplace(magnitude_full_.data(),
                                  peak_hold_full_.data(), N);
            }
            // magnitude_full_ is intentionally left as the live trace - see
            // the real-path comment above.
        }
        return magnitude_full_;
    }

    /// Two-sided peak-hold trace (size = fft_size). Only meaningful after a
    /// process_spectrum_complex() call with peak_hold_enabled() == true.
    const std::vector<float>& peak_hold_trace_complex() const { return peak_hold_full_; }

    /// Two-sided phase output (size = fft_size), produced by the most recent
    /// complex FFT call.
    const std::vector<float>& get_phase_complex() {
        const size_t N = config_.fft_size;
        const size_t half = N / 2;
        phase_full_.resize(N);
        const std::complex<float>* out_c2c = fft_->output_complex();
        for (size_t i = 0; i < N; i++) {
            const size_t src = (i + half) % N;
            phase_full_[i] = std::atan2(out_c2c[src].imag(), out_c2c[src].real());
        }
        return phase_full_;
    }

    // Per-frame spectrum analytics — peak picking, noise floor,
    // fundamental. All take whichever dB-spectrum the caller chooses
    // (live or peak-hold) so markers can lock to the held trace.

    struct SpecPeak { uint16_t bin; float value; float prominence; };

    /// Up to `max_peaks` strongest peaks from a dB spectrum. A bin
    /// must clear an absolute gate AND a local-window prominence
    /// floor, with a minimum bin separation between picks.
    const std::vector<SpecPeak>& detect_peaks(
        std::span<const float> spec,
        size_t max_peaks = 8,
        size_t local_radius = 18,
        float  abs_min_db  = -95.0f,
        float  floor_offset = 18.0f,
        float  local_prom  = 10.0f,
        size_t guard_bins  = 2)
    {
        peaks_.clear();
        const size_t N = spec.size();
        if (N < 2 * local_radius + 1) return peaks_;

        // Global noise-floor estimate via median of finite values.
        scratch_sorted_.assign(spec.begin(), spec.end());
        auto fin_end = std::remove_if(scratch_sorted_.begin(), scratch_sorted_.end(),
                                      [](float v){ return !std::isfinite(v); });
        const size_t fin_n = static_cast<size_t>(fin_end - scratch_sorted_.begin());
        if (fin_n == 0) return peaks_;
        std::sort(scratch_sorted_.begin(), scratch_sorted_.begin() + fin_n);
        const float global_median = scratch_sorted_[fin_n / 2];
        const float gate = std::max(abs_min_db, global_median + floor_offset);
        const size_t min_sep = std::max<size_t>(6, static_cast<size_t>(N * 0.006f));

        // Candidate sweep - same shape-test the JS version used.
        cand_.clear();
        std::vector<float> lb;
        lb.reserve(2 * local_radius);
        for (size_t i = local_radius; i < N - local_radius; i++) {
            const float v = spec[i];
            if (!std::isfinite(v) || v < gate) continue;
            if (!(v >  spec[i-1] && v >= spec[i+1] &&
                  v >  spec[i-2] && v >= spec[i+2])) continue;
            lb.clear();
            for (size_t j = i - local_radius; j <= i + local_radius; j++) {
                if (j == i) continue;
                if ((i > j ? i - j : j - i) <= guard_bins) continue;
                const float lv = spec[j];
                if (std::isfinite(lv)) lb.push_back(lv);
            }
            if (lb.empty()) continue;
            std::nth_element(lb.begin(), lb.begin() + lb.size() / 2, lb.end());
            const float local_floor = lb[lb.size() / 2];
            const float prom = v - local_floor;
            if (prom < local_prom) continue;
            cand_.push_back({static_cast<uint16_t>(i), v, prom});
        }

        // Greedy non-max suppression: take the highest-prominence candidate
        // that isn't within min_sep of an already-chosen peak.
        std::vector<char> used(cand_.size(), 0);
        for (size_t r = 0; r < max_peaks; r++) {
            int best = -1;
            float best_prom = -std::numeric_limits<float>::infinity();
            for (size_t j = 0; j < cand_.size(); j++) {
                if (used[j] || cand_[j].prom <= best_prom) continue;
                bool close = false;
                for (const auto& pk : peaks_) {
                    const size_t d = pk.bin > cand_[j].bin
                        ? pk.bin - cand_[j].bin : cand_[j].bin - pk.bin;
                    if (d < min_sep) { close = true; break; }
                }
                if (!close) { best = static_cast<int>(j); best_prom = cand_[j].prom; }
            }
            if (best < 0) break;
            peaks_.push_back({cand_[best].bin, cand_[best].value, cand_[best].prom});
            used[best] = 1;
        }
        return peaks_;
    }

    /// Median of finite dB bins - a stable noise-floor proxy.
    float estimate_noise_floor(std::span<const float> spec) {
        scratch_sorted_.assign(spec.begin(), spec.end());
        auto fin_end = std::remove_if(scratch_sorted_.begin(), scratch_sorted_.end(),
                                      [](float v){ return !std::isfinite(v); });
        const size_t fin_n = static_cast<size_t>(fin_end - scratch_sorted_.begin());
        if (fin_n == 0) return -200.0f;
        std::nth_element(scratch_sorted_.begin(),
                         scratch_sorted_.begin() + fin_n / 2,
                         scratch_sorted_.begin() + fin_n);
        return scratch_sorted_[fin_n / 2];
    }

    /// Fundamental-frequency estimate from a peak set + sample rate. The
    /// renderer used to use the strongest peak as a stand-in, which is wrong
    /// for harmonic-rich signals (the strongest bin may be the 2nd or 3rd
    /// harmonic). We pick the lowest-frequency peak whose value is within
    /// `tol_db` of the strongest peak - that's the fundamental for a
    /// harmonic series and degenerates to the strongest peak when there's
    /// only one tone. For two-sided spectra we use the offset from -fs/2.
    float estimate_fundamental(const std::vector<SpecPeak>& peaks,
                               size_t N, float sample_rate, bool two_sided,
                               float tol_db = 6.0f) const
    {
        if (peaks.empty()) return 0.0f;
        float strongest = -std::numeric_limits<float>::infinity();
        for (const auto& p : peaks) strongest = std::max(strongest, p.value);
        const SpecPeak* best = nullptr;
        size_t best_bin = std::numeric_limits<size_t>::max();
        for (const auto& p : peaks) {
            if (p.value < strongest - tol_db) continue;
            if (p.bin < best_bin) { best = &p; best_bin = p.bin; }
        }
        if (!best) return 0.0f;
        const float t = (static_cast<float>(best->bin) + 0.5f) / static_cast<float>(N);
        return two_sided ? (t - 0.5f) * sample_rate
                         : t * (sample_rate * 0.5f);
    }

    /// Build amplitude histogram from raw samples.
    const std::vector<float>& compute_histogram(std::span<const float> input,
                                                size_t bins = 256) {
        histogram_.assign(bins, 0.0f);
        for (float s : input) {
            const float v = (s + 1.0f) * 0.5f;  // map [-1,1] -> [0,1]
            const size_t bin = static_cast<size_t>(std::clamp(v, 0.0f, 0.9999f) * bins);
            histogram_[bin] += 1.0f;
        }
        const float peak = *std::max_element(histogram_.begin(), histogram_.end());
        if (peak > 0.0f) {
            for (auto& h : histogram_) h /= peak;
        }
        return histogram_;
    }

    bool clipping_detected() const { return clipping_; }
    size_t spectrum_size() const { return config_.fft_size / 2; }

private:
    Config config_;
    bool clipping_ = false;

    std::vector<float> window_coeffs_;
    std::vector<float> magnitude_;
    std::vector<float> phase_;
    std::vector<float> histogram_;
    std::vector<std::vector<float>> avg_history_;

    // Complex/IQ path (two-sided spectrum, fft_size bins)
    std::vector<float> magnitude_full_;
    std::vector<float> phase_full_;
    std::vector<std::vector<float>> avg_history_full_;

    // Peak hold (per-bin running max). Separate buffers for one-sided / two-sided.
    bool peak_hold_enabled_      = false;
    bool peak_hold_seeded_       = false;
    bool peak_hold_full_seeded_  = false;
    std::vector<float> peak_hold_;
    std::vector<float> peak_hold_full_;

    // Scratch reused by detect_peaks() / estimate_noise_floor() so per-frame
    // calls don't allocate.
    std::vector<float>     scratch_sorted_;
    std::vector<SpecPeak>  peaks_;
    struct CandHolder { uint16_t bin; float value; float prom; };
    std::vector<CandHolder> cand_;

    // FFT backend (pocketfft default; FFTW/KFR optional). Owns input
    // and output buffers; lifetime is tied to the pipeline.
    //   r2c: real input  -> N/2+1 complex bins (one-sided spectrum).
    //   c2c: complex in  -> N complex bins (two-sided spectrum, IQ path).
    std::unique_ptr<ss::fft::IFftBackend> fft_;

    // Overlap state. `overlap_factor_` is 1 (no overlap), 2 (50%), or 4 (75%).
    // Slide buffers retain `fft_size − hop_size` samples between calls so
    // each FFT sees a window that overlaps the previous one - gives finer
    // time resolution on the spectrogram without changing fft_size.
    int overlap_factor_ = 1;
    std::vector<float> slide_real_;
    std::vector<float> slide_re_;
    std::vector<float> slide_im_;

    // Window calibration. coherent_gain_ is the DC response of the active
    // window (mean of its coefficients): rectangular -> 1, Hanning -> 0.5,
    // Blackman -> 0.42, etc. Dividing the FFT magnitude by this constant
    // makes a unit-amplitude tone read 0 dBFS regardless of window choice
    // - without it, switching from Rectangular to Hanning drops every
    // tone by 6 dB and the user has to mentally compensate.
    // enbw_bins_ is the equivalent noise bandwidth (in bins) of the
    // active window - useful for PSD-mode normalization but not applied
    // by default (we calibrate for tones, not noise PSD).
    float coherent_gain_ = 1.0f;
    float enbw_bins_     = 1.0f;

    // Multitaper state. taper_count_ is in {1, 2, 4, 8}. tapers_ holds
    // K vectors of length fft_size - sine tapers, normalized to unit
    // L2 norm. mt_cg_ is the RMS coherent gain across the K tapers,
    // used as the normalization divisor in MT mode so a unit-amplitude
    // tone reads near 0 dBFS regardless of K.
    int taper_count_ = 1;
    std::vector<std::vector<float>> tapers_;
    float mt_cg_ = 1.0f;
    // Scratch accumulator for the K-FFT averaged squared magnitudes.
    std::vector<float> psd_acc_;
    std::vector<float> psd_acc_full_;

    void resize(size_t n) {
        window_coeffs_.resize(n);
        magnitude_.resize(n / 2);
        avg_history_.clear();
        avg_history_full_.clear();
        // Slide buffers must match the current fft_size; resizing zeros
        // them so an FFT-size change doesn't carry stale samples through
        // the overlap path.
        slide_real_.assign(n, 0.0f);
        slide_re_.assign(n,   0.0f);
        slide_im_.assign(n,   0.0f);
        psd_acc_.assign(n / 2, 0.0f);
        psd_acc_full_.assign(n, 0.0f);

        compute_window();
        compute_tapers();

        // Backend (re)allocates input/output buffers and any plan state
        // it needs. FFTW will use cached wisdom on the fast path; the
        // pocketfft and KFR backends have no analogue and just allocate.
        fft_->resize(n);
    }

    void compute_window() {
        const size_t N = config_.fft_size;
        constexpr double pi  = std::numbers::pi;
        constexpr double pi2 = 2.0 * pi;

        switch (config_.window) {
        case WindowFunction::Rectangular:
            std::fill(window_coeffs_.begin(), window_coeffs_.end(), 1.0f);
            break;

        case WindowFunction::Hanning:
            for (size_t i = 0; i < N; i++)
                window_coeffs_[i] = 0.5f * (1.0f - std::cos(pi2 * i / (N - 1)));
            break;

        case WindowFunction::Hamming:
            for (size_t i = 0; i < N; i++)
                window_coeffs_[i] = 0.54f - 0.46f * std::cos(pi2 * i / (N - 1));
            break;

        case WindowFunction::Blackman:
            for (size_t i = 0; i < N; i++)
                window_coeffs_[i] = 0.42f - 0.5f * std::cos(pi2 * i / (N - 1))
                                   + 0.08f * std::cos(4.0 * pi * i / (N - 1));
            break;

        case WindowFunction::FlatTop:
            for (size_t i = 0; i < N; i++) {
                double x = pi2 * i / (N - 1);
                window_coeffs_[i] = static_cast<float>(
                    0.21557895 - 0.41663158 * std::cos(x)
                    + 0.277263158 * std::cos(2 * x)
                    - 0.083578947 * std::cos(3 * x)
                    + 0.006947368 * std::cos(4 * x)
                );
            }
            break;

        case WindowFunction::Kaiser: {
            // Kaiser with beta = 8.0 (good sidelobe suppression)
            const double beta = 8.0;
            const double denom = bessel_i0(beta);
            for (size_t i = 0; i < N; i++) {
                double r = 2.0 * i / (N - 1) - 1.0;
                window_coeffs_[i] = static_cast<float>(
                    bessel_i0(beta * std::sqrt(1.0 - r * r)) / denom
                );
            }
            break;
        }
        default:
            std::fill(window_coeffs_.begin(), window_coeffs_.end(), 1.0f);
        }

        // Coherent gain = mean(coeffs); ENBW (in bins) = N · sum(coeffs²) /
        // sum(coeffs)². Both are well-known per-window constants; computing
        // them here keeps the FFT path window-independent for tone amplitude.
        double sum  = 0.0;
        double sum2 = 0.0;
        for (size_t i = 0; i < N; i++) {
            const double w = window_coeffs_[i];
            sum  += w;
            sum2 += w * w;
        }
        coherent_gain_ = (sum  > 1e-12) ? static_cast<float>(sum / N) : 1.0f;
        enbw_bins_     = (sum  > 1e-12) ? static_cast<float>(N * sum2 / (sum * sum)) : 1.0f;
    }

    // Build K orthogonal sine tapers of length fft_size, each unit-L2.
    // Riedel-Sidorenko form: w_k(n) = sqrt(2/(N+1)) · sin(π·(k+1)·(n+1)/(N+1)).
    // Stores them in tapers_ and caches mt_cg_, the RMS coherent gain
    // across tapers, which the MT FFT path divides by so tone amplitude
    // stays close to single-window calibration.
    void compute_tapers() {
        const size_t N = config_.fft_size;
        const int    K = taper_count_;
        tapers_.assign(K, std::vector<float>(N, 0.0f));
        const double amp = std::sqrt(2.0 / static_cast<double>(N + 1));
        constexpr double pi = std::numbers::pi;
        double cg_sq_sum = 0.0;
        for (int k = 0; k < K; k++) {
            double cg_sum = 0.0;
            for (size_t n = 0; n < N; n++) {
                const double v = amp * std::sin(pi * (k + 1) * (n + 1) /
                                                static_cast<double>(N + 1));
                tapers_[k][n] = static_cast<float>(v);
                cg_sum += v;
            }
            const double cg_k = cg_sum / static_cast<double>(N);
            cg_sq_sum += cg_k * cg_k;
        }
        const double mt_cg_sq = cg_sq_sum / static_cast<double>(K);
        // Avoid divide-by-zero when sine-taper coherent gain underflows
        // for very large N - never happens in practice but the guard
        // costs nothing.
        mt_cg_ = (mt_cg_sq > 1e-30) ? static_cast<float>(std::sqrt(mt_cg_sq))
                                    : 1.0f;
    }

    /// Modified Bessel function of the first kind, order 0
    static double bessel_i0(double x) {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 30; k++) {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
            if (term < 1e-12 * sum) break;
        }
        return sum;
    }
};

} // namespace signalscope
