#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>      // only used by the signal handler (async-signal-safe printf)
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <cmath>
#include <iostream>
#include <limits>
#include <mutex>
#include <span>
#include <string>
#include <syncstream>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

// WebSocket implementation headers must live at file scope - never inside a
// namespace, or STL names they include will be resolved as e.g. ns::std::*.
#if !defined(USE_SIMPLE_WS)
#  include <uwebsockets/App.h>
#else
#  include <arpa/inet.h>
#  include <cerrno>
#  include <cstdlib>
#  include <fcntl.h>
#  include <iomanip>
#  include <netinet/in.h>
#  include <openssl/sha.h>
#  include <poll.h>
#  include <sstream>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

#include "nlohmann/json.hpp"

#include "protocol/protocol.hpp"
#include "protocol/ws_framing.hpp"
#include "util/ring_buffer.hpp"
#include "dsp/fft/fft_backend.hpp"
#include "dsp/dsp_pipeline.hpp"
#include "dsp/simd_ops.hpp"
#include "dsp/downconverter.hpp"
#include "dsp/hilbert.hpp"
#include "dsp/weighting.hpp"
#include "dsp/spectrum_history.hpp"
#include "util/file_exports.hpp"
#include "source/iq_listener.hpp"
#include "audio/audio_capture.hpp"
#include "audio/file_source.hpp"

using json = nlohmann::json;

namespace signalscope
{
    // Common FFT sizes for the background warmup pass. Pocketfft is a no-op
    // here (no plans to cache); FFTW uses these to populate wisdom so
    // set_fft_size never blocks on a cold machine.
    static const std::array<size_t, 9> WARMUP_FFT_SIZES = {
        256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536
    };

    // Used as the warmup cancellation predicate. Set by main() before the
    // warmup thread starts; read from the warmup thread.
    static std::atomic<bool> *g_warmup_running = nullptr;
    static bool warmup_should_cancel() {
        return !g_warmup_running ||
               !g_warmup_running->load(std::memory_order_relaxed);
    }

    static void fft_warmup_thread() {
        ss::fft::warmup(std::span<const size_t>(WARMUP_FFT_SIZES),
                        warmup_should_cancel);
    }

    struct AppState
    {
        std::atomic<bool> running{true};
        std::atomic<bool> paused{false};
        std::atomic<size_t> frame_counter{0};

        // Set by ws_thread_func if listen/bind fails. Main thread polls
        // this after spawning ws_thread_func and shuts down cleanly if
        // it's set. Stable line "[ws] fatal:" is written to stderr in
        // the same code path so Electron's stdout parser can detect it.
        std::atomic<bool> ws_fatal{false};

        // Source mode: "mic", "test", "file"
        std::string source_mode = "test";
        std::string source_file;
        std::mutex source_mutex;

        // File source - owns its own thread that pumps into capture_buffer.
        FileSource file_source;

        // Network IQ listener - accepts a TCP publisher on a configurable
        // port and pumps decoded samples into capture_buffer the same way
        // FileSource does. See iq_listener.hpp for the wire format.
        IqListenerSource iq_listener;
        IqListenerSource::Defaults listen_defaults;
        std::atomic<uint16_t> listen_port{5555};
        std::atomic<bool>   source_is_complex{false}; // true while playing complex/IQ files
        std::atomic<size_t> source_loop_count{0};
        // Center frequency (Hz). Non-zero only for SDR/SigMF captures that
        // declared core:frequency. Used by the renderer to label absolute
        // frequencies on the spectrum/spectrogram axes.
        std::atomic<float>  source_center_freq{0.0f};

        // Ring buffer: capture thread -> DSP thread
        SPSCRingBuffer<float> capture_buffer{1 << 20}; // ~1M samples ≈ 21s at 48kHz

        // DSP pipeline (guarded by dsp_mutex - WS thread and DSP thread both access it)
        DSPPipeline dsp;
        std::mutex dsp_mutex;

        // Digital down-converter - guarded by the same mutex; reconfigured
        // by the WS thread when the user moves Tune/Decimate. When inactive
        // (tune=0, M=1) the DSP loop bypasses it entirely.
        DownConverter ddc;

        // Optional Hilbert transformer placed in front of the DDC when the
        // source is real. Suppresses the conjugate-symmetric mirror images
        // a real-input DDC otherwise produces. Bypassed when:
        //   - the user has the toggle off, OR
        //   - the source is already complex (an analytic input doesn't need
        //     it and would be attenuated by the FIR's band-edge rolloff).
        HilbertTransformer hilbert;
        std::atomic<bool>  suppress_image{false};

        // Peak-detector absolute-level gate (dBFS). A candidate bin must be
        // ≥ this value to qualify, matching the spectrum y-axis the user is
        // looking at - "show me anything above −26.5 dB" maps directly. The
        // separate prominence rise (local_prom) stays at its default so
        // plateaus of noise don't all qualify as peaks.
        std::atomic<float> peak_min_level{-95.0f};

        // Equalization curve applied to the spectrum dB values before
        // peak detection and serialization. None = raw spectrum;
        // A/C = IEC 61672 weightings; Flatten is renderer-only y-axis
        // auto-range - backend treats it as None. Stored as int so it's
        // trivially atomic without a wrapper.
        std::atomic<int> weighting{static_cast<int>(Weighting::None)};

        // Deep history ring - backend-side storage of recent spectrum
        // frames so the renderer can scroll back beyond its GPU texture
        // working set. See dsp/spectrum_history.hpp.
        SpectrumHistory history;

        AudioCapture audio;

        // All output frames are produced by the DSP thread and consumed
        // by the WS broadcast loop. They share one mutex so the producer
        // can swap the whole set atomically and the consumer sees a
        // consistent snapshot. `ready` is the wake signal - when true,
        // the WS loop drains every populated buffer once.
        // Grouped here (rather than as 11 sibling fields on AppState)
        // so the locking contract is self-documenting.
        struct OutputFrames {
            std::mutex mu;
            std::vector<uint8_t> spec;     // spectrum (live)
            std::vector<uint8_t> hold;     // peak-hold trace (only when enabled)
            std::vector<uint8_t> wave;     // waveform
            std::vector<uint8_t> phase;    // phase
            std::vector<uint8_t> hist;     // histogram
            std::vector<uint8_t> iq;       // IQ
            std::vector<uint8_t> status;   // source status (loop counter, etc.)
            std::vector<uint8_t> stats;    // peaks + noise floor + fundamental
            // Pending history-query response frames. Built by the
            // request_history handler and drained by the WS send loop
            // on the next frame_ready cycle.
            std::vector<std::vector<uint8_t>> history;
            // Retired history-row buffers — refilled in place by the
            // request_history handler so each batch of historical frames
            // doesn't trigger N fresh allocations. Same locking contract
            // as the other OutputFrames members.
            std::vector<std::vector<uint8_t>> history_pool;
            std::atomic<bool> ready{false};
        };
        OutputFrames outputs;

        int ws_port = 8765;
        size_t target_fps = 60;
        float sample_rate = 48000.0f;
    };

    // Worker-thread console writes use std::osyncstream (C++20 <syncstream>) so
    // that operator<< chains from different threads can't interleave on the same
    // stream. Main-thread writes (the device listing + version banner near the
    // bottom of this file) do not need it -- they run before any worker thread
    // starts.

    // DSP Thread
    // Pulls samples, runs FFT, writes output frames.
    void dsp_thread_func(AppState &state)
    {
        std::osyncstream(std::cout) << "[ss] DSP thread started" << std::endl;

        const auto frame_interval = std::chrono::microseconds(
            1000000 / state.target_fps);

        // Per-iteration scratch - kept at function scope (not as
        // `static thread_local` inside branches) so lifetimes are visible.
        //   input_buffer : real samples handed to DSPPipeline (for IQ
        //                  sources, holds the I channel for waveform/histogram).
        //   interleaved  : I,Q,I,Q staging for complex-source reads.
        //   raw_iq_*     : de-interleaved IQ before DDC.
        //   h_re/h_im    : Hilbert analytic output when image suppression is on.
        struct DspScratch {
            std::vector<float> input_buffer;
            std::vector<float> test_buffer;
            std::vector<float> interleaved;
            std::vector<float> raw_iq_re;
            std::vector<float> raw_iq_im;
            std::vector<float> h_re;
            std::vector<float> h_im;
        } scratch;
        auto& input_buffer = scratch.input_buffer;
        auto& test_buffer  = scratch.test_buffer;

        while (state.running.load(std::memory_order_relaxed))
        {
            if (state.paused.load(std::memory_order_acquire))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            auto frame_start = std::chrono::steady_clock::now();

            // Listen-mode: when a header has landed, sync its effective
            // sample-rate / center-freq / format-implied complex flag back
            // into AppState so the DSP path and outgoing frames pick them up.
            if (state.iq_listener.state() == IqListenerSource::State::Connected) {
                const uint32_t lsr = state.iq_listener.effective_sample_rate();
                if (lsr > 0 && static_cast<float>(lsr) != state.sample_rate) {
                    std::lock_guard<std::mutex> dlock(state.dsp_mutex);
                    state.sample_rate = static_cast<float>(lsr);
                    state.dsp.set_sample_rate(state.sample_rate);
                    state.ddc.configure(state.ddc.tune_hz(),
                                        state.ddc.decimation(), state.sample_rate);
                }
                state.source_is_complex.store(state.iq_listener.is_complex(),
                                              std::memory_order_release);
                state.source_center_freq.store(
                    static_cast<float>(state.iq_listener.effective_center_freq()),
                    std::memory_order_release);
            }

            size_t fft_size;
            size_t hop_size;
            bool   ddc_active;
            size_t ddc_decim;
            float  ddc_tune;
            {
                std::lock_guard<std::mutex> lock(state.dsp_mutex);
                fft_size   = state.dsp.config().fft_size;
                hop_size   = state.dsp.hop_size();
                ddc_active = state.ddc.active();
                ddc_decim  = state.ddc.decimation();
                ddc_tune   = state.ddc.tune_hz();
            }
            const bool source_complex = state.source_is_complex.load(
                std::memory_order_acquire);
            const bool suppress_image = state.suppress_image.load(
                std::memory_order_acquire);
            // After DDC the output is complex when:
            //   - the source is already complex, OR
            //   - we're applying a non-zero frequency shift (NCO produces
            //     non-zero imag), OR
            //   - the Hilbert pre-stage is engaged (input becomes analytic,
            //     so even with tune=0 the imag component is non-zero).
            // With tune=0 AND no Hilbert AND a real source, the imag output
            // is identically zero and we keep the real FFT path so the user
            // sees a one-sided spectrum without conjugate-symmetric mirrors.
            const bool out_complex = source_complex
                || (ddc_active && ddc_tune != 0.0f)
                || (ddc_active && !source_complex && suppress_image);

            // Number of source samples needed per FFT block. With DDC active
            // the decimator collapses M source samples into 1 output sample,
            // so we read M·fft_size up front. The capture ring buffer is
            // ~1M floats (~21s @ 48kHz) so even M=64 fits comfortably.
            // With overlap engaged the FFT is fed `hop_size` fresh samples
            // per call (the rest is retained inside DSPPipeline's slide
            // buffer), so we only pull `hop_size` × decimation source
            // samples per iteration. With overlap=1 (the default) hop_size
            // == fft_size and the source pull is unchanged from the
            // historical behavior.
            const size_t needed = ddc_active ? hop_size * ddc_decim : hop_size;
            input_buffer.resize(needed);

            // Output FFT-input arrays sized to hop_size - DSPPipeline owns
            // the slide window that turns this back into a full fft_size
            // FFT input.
            std::vector<float> iq_re, iq_im;
            iq_re.resize(hop_size);
            iq_im.resize(hop_size);

            // Pull samples from ring buffer or generate test signal.
            // Reads are gated on available() so partial chunks accumulate
            // in the ring instead of being consumed and discarded — at
            // large FFT sizes (e.g. 32768) the file source pumps in
            // smaller blocks than the FFT needs, so reading "what's
            // there" would burn samples and never produce a frame.
            size_t got = 0;
            {
                std::lock_guard<std::mutex> lock(state.source_mutex);
                if (state.source_mode == "test")
                {
                    test_buffer.resize(needed);
                    state.audio.generate_test_signal(
                        test_buffer.data(), needed,
                        440.0f + 100.0f * std::sin(
                                              state.frame_counter.load() * 0.01f));
                    std::memcpy(input_buffer.data(), test_buffer.data(),
                                needed * sizeof(float));
                    got = needed;
                }
                else if (source_complex)
                {
                    // File source pushes interleaved I,Q,I,Q, ... so we need
                    // 2*needed floats to fill a (possibly oversampled) block.
                    scratch.interleaved.resize(needed * 2);
                    if (state.capture_buffer.available() >= needed * 2) {
                        const size_t got_floats = state.capture_buffer.read(
                            scratch.interleaved.data(), needed * 2);
                        got = got_floats / 2;
                    }
                    scratch.raw_iq_re.resize(needed);
                    scratch.raw_iq_im.resize(needed);
                    for (size_t i = 0; i < got; i++) {
                        scratch.raw_iq_re[i] = scratch.interleaved[i * 2];
                        scratch.raw_iq_im[i] = scratch.interleaved[i * 2 + 1];
                    }
                    // input_buffer holds the I-channel as the "representative"
                    // real signal for waveform/histogram. Sized to needed so
                    // the post-DDC fill below has room.
                    for (size_t i = 0; i < got; i++) input_buffer[i] = scratch.raw_iq_re[i];
                }
                else
                {
                    if (state.capture_buffer.available() >= needed) {
                        got = state.capture_buffer.read(
                            input_buffer.data(), needed);
                    }
                }
            }

            if (got < needed)
            {
                // Not enough data yet - wait
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }

            // Route through DDC if active. Output is `hop_size` complex
            // samples in iq_re/iq_im. We also overwrite input_buffer with
            // the I component so waveform/histogram displays show the
            // (decimated) baseband.
            // Hilbert pre-stage: when the source is real and the user has
            // "suppress image" on, we first convert input -> analytic signal,
            // then feed the (now complex) signal to ddc.process_complex.
            // The DDC's NCO mixer no longer sees a conjugate-symmetric
            // input, so each tone produces exactly one peak after tuning.
            const bool use_hilbert = ddc_active && !source_complex && suppress_image;
            if (ddc_active) {
                std::lock_guard<std::mutex> lock(state.dsp_mutex);
                const std::span<const float> raw_re{scratch.raw_iq_re.data(), needed};
                const std::span<const float> raw_im{scratch.raw_iq_im.data(), needed};
                const std::span<const float> input{input_buffer.data(), needed};
                if (source_complex) {
                    state.ddc.process_complex(raw_re, raw_im, iq_re, iq_im);
                } else if (use_hilbert) {
                    state.hilbert.process(input, scratch.h_re, scratch.h_im);
                    state.ddc.process_complex(scratch.h_re, scratch.h_im,
                                              iq_re, iq_im);
                } else {
                    state.ddc.process_real(input, iq_re, iq_im);
                }
                input_buffer.resize(hop_size);
                std::memcpy(input_buffer.data(), iq_re.data(),
                            hop_size * sizeof(float));
            } else if (source_complex) {
                // Pass-through: copy hop_size complex samples from raw.
                std::memcpy(iq_re.data(), scratch.raw_iq_re.data(),
                            hop_size * sizeof(float));
                std::memcpy(iq_im.data(), scratch.raw_iq_im.data(),
                            hop_size * sizeof(float));
                input_buffer.resize(hop_size);
            }
            // else: real source + no DDC - input_buffer already has the
            // hop_size real samples we need.

            // Effective sample rate / center frequency seen by the FFT and
            // reported on the wire. With DDC active, the spectrum is the
            // [-fs/(2M), +fs/(2M)] baseband band centred at source_cf+tune.
            const float effective_sr = ddc_active
                ? state.ddc.effective_sample_rate()
                : state.sample_rate;
            const float effective_cf =
                state.source_center_freq.load(std::memory_order_relaxed)
                + (ddc_active ? ddc_tune : 0.0f);

            // Run DSP - hold dsp_mutex for the entire processing block so that
            // WS-thread config changes (set_gain, set_fft_size, etc.) don't race.
            std::vector<float> spectrum_vec, phase_vec, histogram_vec, hold_vec;
            std::vector<float> stats_payload;
            bool clipping;
            bool peak_hold_active = false;
            {
                std::lock_guard<std::mutex> lock(state.dsp_mutex);
                // Each call delivers `hop_size` fresh samples; DSPPipeline
                // slides them onto its retained window when overlap > 1
                // and FFTs the full fft_size internally.
                const std::span<const float> hop_input{input_buffer.data(), hop_size};
                if (out_complex) {
                    const std::span<const float> hop_re{iq_re.data(), hop_size};
                    const std::span<const float> hop_im{iq_im.data(), hop_size};
                    const auto &spectrum = state.dsp.process_spectrum_complex(
                        hop_re, hop_im);
                    spectrum_vec.assign(spectrum.begin(), spectrum.end());
                    const auto &phase = state.dsp.get_phase_complex();
                    phase_vec.assign(phase.begin(), phase.end());
                    if (state.dsp.peak_hold_enabled()) {
                        const auto &hold = state.dsp.peak_hold_trace_complex();
                        hold_vec.assign(hold.begin(), hold.end());
                        peak_hold_active = true;
                    }
                } else {
                    const auto &spectrum = state.dsp.process_spectrum(hop_input);
                    spectrum_vec.assign(spectrum.begin(), spectrum.end());
                    const auto &phase = state.dsp.get_phase();
                    phase_vec.assign(phase.begin(), phase.end());
                    if (state.dsp.peak_hold_enabled()) {
                        const auto &hold = state.dsp.peak_hold_trace();
                        hold_vec.assign(hold.begin(), hold.end());
                        peak_hold_active = true;
                    }
                }
                clipping = state.dsp.clipping_detected();
                const auto &histogram = state.dsp.compute_histogram(hop_input);
                histogram_vec.assign(histogram.begin(), histogram.end());

                // Equalization (A/C-weight). Applied AFTER peak-hold capture
                // so the held trace stays in absolute dBFS and toggling the
                // curve doesn't mutate the running maximum. Flatten is handled
                // by the renderer as a y-axis auto-range - backend leaves the
                // dB values untouched so peaks/noise-floor stay in absolute units.
                const Weighting wt = static_cast<Weighting>(
                    state.weighting.load(std::memory_order_acquire));
                if (wt != Weighting::None && wt != Weighting::Flatten) {
                    apply_weighting(spectrum_vec, wt, effective_sr, out_complex);
                    if (peak_hold_active) {
                        apply_weighting(hold_vec, wt, effective_sr, out_complex);
                    }
                }

                // Stats: peaks + noise floor + fundamental. When peak hold is
                // on we run the picker against the held trace so the markers
                // anchor to the steady envelope and stop jumping during bursts;
                // the live spectrum is still drawn underneath. Fundamental
                // uses the effective sample rate so the Hz it reports matches
                // the spectrum frame's axis.
                const std::vector<float>& stat_src =
                    peak_hold_active ? hold_vec : spectrum_vec;
                const float min_level = state.peak_min_level.load(
                    std::memory_order_acquire);
                const auto& peaks = state.dsp.detect_peaks(
                    stat_src,
                    /*max_peaks*/    8,
                    /*local_radius*/ 18,
                    /*abs_min_db*/   min_level,
                    /*floor_offset*/ 18.0f,
                    /*local_prom*/   10.0f);
                const float nf = state.dsp.estimate_noise_floor(stat_src);
                const float fund = state.dsp.estimate_fundamental(
                    peaks, stat_src.size(), effective_sr, out_complex);

                // Min/max of the LIVE spectrum (not hold), used by the
                // renderer's Flatten auto-range so it doesn't iterate the
                // full spectrum every RAF tick just to find extrema.
                // Held off the noise floor: -1e30 sentinels appear in
                // bins the backend deliberately silenced (e.g. DC for
                // two-sided plots) and we skip those so they don't pull
                // the floor to negative infinity.
                // SIMD-vectorized min/max with sentinel exclusion. The
                // backend writes ±1e30f to deliberately silenced bins
                // (e.g. DC in two-sided plots); those must not pull the
                // running min/max. simd::min_max_filtered's scalar
                // reference matches the prior loop's semantics 1:1.
                float spec_min, spec_max;
                signalscope::simd::min_max_filtered(
                    spectrum_vec.data(), spectrum_vec.size(),
                    -1e30f, 1e30f, spec_min, spec_max);
                if (!std::isfinite(spec_min) || !std::isfinite(spec_max)
                    || spec_max <= spec_min) {
                    spec_min = -130.0f;
                    spec_max = 10.0f;
                }

                // [noise_floor, fundamental_hz, K, bin0, val0, prom0, ...,
                //  spec_min_db, spec_max_db]
                // Trailing min/max are optional - older renderers ignore
                // them; the current renderer reads them when present.
                stats_payload.clear();
                stats_payload.reserve(3 + 3 * peaks.size() + 2);
                stats_payload.push_back(nf);
                stats_payload.push_back(fund);
                stats_payload.push_back(static_cast<float>(peaks.size()));
                for (const auto& p : peaks) {
                    stats_payload.push_back(static_cast<float>(p.bin));
                    stats_payload.push_back(p.value);
                    stats_payload.push_back(p.prominence);
                }
                stats_payload.push_back(spec_min);
                stats_payload.push_back(spec_max);
            }
            uint32_t frame_id = state.frame_counter.fetch_add(1);

            // Build IQ panel data - planar layout: first iq_len floats are I,
            // next iq_len are Q. Renderer can take two subarrays directly with
            // no per-sample loop. Capped at hop_size since iq_re/im and
            // input_buffer hold one hop of fresh samples per iteration.
            size_t iq_len = std::min<size_t>(hop_size / 2, 512);
            // For real-source stride we read input[i*2] / input[i*2+1], so
            // we need 2·iq_len addressable elements.
            if (!out_complex) iq_len = std::min(iq_len, hop_size / 2);
            std::vector<float> iq_data(iq_len * 2);
            float* iq_i = iq_data.data();
            float* iq_q = iq_data.data() + iq_len;
            if (out_complex) {
                std::memcpy(iq_i, iq_re.data(), iq_len * sizeof(float));
                std::memcpy(iq_q, iq_im.data(), iq_len * sizeof(float));
            } else {
                // Real source: stride the input so we don't paint a perfect
                // diagonal in the IQ panel (matches old approximation).
                for (size_t i = 0; i < iq_len; i++) {
                    iq_i[i] = input_buffer[i * 2];
                    iq_q[i] = input_buffer[i * 2 + 1];
                }
            }

            // Compose flag byte. The Complex bit reflects the SOURCE (file
            // IQ / SDR), not the FFT-output shape - the renderer uses it to
            // gate features that only make sense for inherently-complex
            // sources (e.g. disabling the Hilbert pre-stage). TwoSided
            // reflects whether the spectrum spans -fs/2..+fs/2, which can
            // be true for a real source after DDC mixing or after a Hilbert
            // pre-stage.
            uint8_t flags = clipping ? static_cast<uint8_t>(FrameFlags::Clipping) : 0;
            if (source_complex) {
                flags |= static_cast<uint8_t>(FrameFlags::Complex);
            }
            const uint8_t spec_flags = flags |
                (out_complex ? static_cast<uint8_t>(FrameFlags::TwoSided) : 0);

            // Serialize all frames. Spectrum-axis frames carry the EFFECTIVE
            // sample rate (post-DDC) so the renderer's axis labels span the
            // current band; waveform/histogram/IQ also reference effective_sr
            // because after decimation they describe the baseband, not the
            // raw input.
            //
            // We serialize directly into the OutputFrames buffers (under the
            // mutex) so the hot path doesn't allocate after warm-up. The WS
            // thread swaps these buffers out under the same mutex.
            {
                std::lock_guard<std::mutex> lock(state.outputs.mu);

                serialize_frame_into(
                    state.outputs.spec,
                    FrameType::Spectrum, spectrum_vec.data(), spectrum_vec.size(),
                    static_cast<uint16_t>(fft_size), frame_id,
                    effective_sr, spec_flags);

                // Capture into the deep-history ring. The renderer's GPU
                // texture only holds the recent working set; this ring is
                // what the user pans into when they pause and scroll back.
                // Captured AFTER weighting/peak-hold so historical rows match
                // exactly what was on screen at the time.
                state.history.push(spectrum_vec, effective_sr, spec_flags);

                // Peak-hold trace as its own frame so the spectrogram (which
                // uses the live Spectrum frame) is unaffected. We clear when
                // hold is off so an empty buffer signals "skip" downstream.
                if (peak_hold_active) {
                    serialize_frame_into(
                        state.outputs.hold,
                        FrameType::SpectrumHold, hold_vec.data(), hold_vec.size(),
                        static_cast<uint16_t>(fft_size), frame_id,
                        effective_sr, spec_flags);
                } else {
                    state.outputs.hold.clear();
                }

                // Waveform shows the most recent samples. With overlap engaged
                // input_buffer holds only one hop, so wave_len caps at hop_size.
                size_t wave_len = std::min<size_t>(hop_size, 1024);
                serialize_frame_into(
                    state.outputs.wave,
                    FrameType::Waveform,
                    input_buffer.data() + (hop_size - wave_len), wave_len,
                    static_cast<uint16_t>(fft_size), frame_id,
                    effective_sr, flags);

                serialize_frame_into(
                    state.outputs.phase,
                    FrameType::Phase, phase_vec.data(), phase_vec.size(),
                    static_cast<uint16_t>(fft_size), frame_id,
                    effective_sr, spec_flags);

                serialize_frame_into(
                    state.outputs.hist,
                    FrameType::Histogram, histogram_vec.data(), histogram_vec.size(),
                    static_cast<uint16_t>(fft_size), frame_id,
                    effective_sr, flags);

                serialize_frame_into(
                    state.outputs.iq,
                    FrameType::IQ, iq_data.data(), iq_data.size(),
                    static_cast<uint16_t>(fft_size), frame_id,
                    effective_sr, flags);

                // Source-status frame: report the EFFECTIVE complex flag and
                // EFFECTIVE center frequency so axis labels use absolute Hz.
                float status_payload[4] = {
                    static_cast<float>(state.file_source.loop_count()),
                    state.source_mode == "file"   ? 2.0f
                        : state.source_mode == "mic"    ? 1.0f
                        : state.source_mode == "listen" ? 3.0f
                        : 0.0f,
                    source_complex ? 1.0f : 0.0f,
                    effective_cf,
                };
                state.source_loop_count.store(
                    static_cast<size_t>(status_payload[0]),
                    std::memory_order_relaxed);
                serialize_frame_into(
                    state.outputs.status,
                    FrameType::SourceStatus, status_payload, 4,
                    static_cast<uint16_t>(fft_size), frame_id,
                    effective_sr, flags);

                serialize_frame_into(
                    state.outputs.stats,
                    FrameType::SpectrumStats,
                    stats_payload.data(), stats_payload.size(),
                    static_cast<uint16_t>(fft_size), frame_id,
                    effective_sr, spec_flags);

                state.outputs.ready.store(true, std::memory_order_release);
            }

            // Frame rate limiting
            auto elapsed = std::chrono::steady_clock::now() - frame_start;
            if (elapsed < frame_interval)
            {
                std::this_thread::sleep_for(frame_interval - elapsed);
            }
        }

        std::osyncstream(std::cout) << "[ss] DSP thread stopped" << std::endl;
    }

    // Command dispatch — one function per command, registered in
    // command_table() below. Lock acquisition is per-handler (the
    // pattern varies: dsp_mutex, source_mutex, both, neither).
    // handle_command() wraps each call in a try/catch so a malformed
    // payload can't take the WS thread down.
    namespace cmd {

    static std::string current_source_mode(AppState& state) {
        std::lock_guard<std::mutex> g(state.source_mutex);
        return state.source_mode;
    }
    // Reset capture_buffer too so the prior source's tail can't bleed
    // into the first frame from the new publisher.
    static void rebind_listener(AppState& state, uint16_t port) {
        state.iq_listener.stop();
        state.capture_buffer.reset();
        if (!state.iq_listener.start(port, state.listen_defaults,
                                     state.capture_buffer)) {
            std::osyncstream(std::cerr) << "[ws] iq_listener.start failed: "
                                        << state.iq_listener.last_error() << '\n';
        }
    }

    static void set_fft_size(AppState& state, const json& j) {
        std::lock_guard<std::mutex> lock(state.dsp_mutex);
        state.dsp.set_fft_size(j.value("value", std::size_t{4096}));
        // History rows are fixed-width on the renderer texture - drop
        // on resize rather than mix bin counts.
        state.history.clear();
    }
    static void set_window(AppState& state, const json& j) {
        std::lock_guard<std::mutex> lock(state.dsp_mutex);
        state.dsp.set_window(j.value("value", std::string{}));
    }
    static void set_gain(AppState& state, const json& j) {
        std::lock_guard<std::mutex> lock(state.dsp_mutex);
        state.dsp.set_gain(j.value("value", 0.0f));
    }
    static void set_avg_count(AppState& state, const json& j) {
        std::lock_guard<std::mutex> lock(state.dsp_mutex);
        state.dsp.set_avg_count(j.value("value", std::size_t{1}));
    }
    static void set_sample_rate(AppState& state, const json& j) {
        const float sr = j.value("value", 48000.0f);
        state.sample_rate = sr;
        std::lock_guard<std::mutex> lock(state.dsp_mutex);
        state.dsp.set_sample_rate(sr);
        // DDC NCO + FIR cutoff are source_sr-dependent.
        state.ddc.configure(state.ddc.tune_hz(), state.ddc.decimation(), sr);
    }
    static void set_pause(AppState& state, const json& j) {
        state.paused.store(j.value("value", false), std::memory_order_release);
    }
    static void set_peak_hold(AppState& state, const json& j) {
        std::lock_guard<std::mutex> lock(state.dsp_mutex);
        state.dsp.set_peak_hold(j.value("value", false));
    }
    static void clear_peak_hold(AppState& state, const json& /*j*/) {
        std::lock_guard<std::mutex> lock(state.dsp_mutex);
        state.dsp.clear_peak_hold();
    }
    static void set_peak_min_level(AppState& state, const json& j) {
        // Clamp to the nominal dBFS range pinned by the colour mapping.
        float v = j.value("value", -95.0f);
        v = std::clamp(v, -130.0f, 0.0f);
        state.peak_min_level.store(v, std::memory_order_release);
    }
    static void request_history(AppState& state, const json& j) {
        // Emits one HistoryRow per row; the frame header's frame_id
        // carries the row's offset so the renderer can place it
        // independent of arrival order.
        const std::size_t start = j.value("start", std::size_t{0});
        const std::size_t count = j.value("count", std::size_t{0});
        std::vector<SpectrumHistory::Row> rows;
        state.history.fetch(start, count, rows);
        std::lock_guard<std::mutex> lock(state.outputs.mu);
        for (std::size_t i = 0; i < rows.size(); i++) {
            const auto& r = rows[i];
            // Reuse a retired row buffer when one is available.
            std::vector<uint8_t> row_buf;
            if (!state.outputs.history_pool.empty()) {
                row_buf = std::move(state.outputs.history_pool.back());
                state.outputs.history_pool.pop_back();
            }
            serialize_frame_into(
                row_buf,
                FrameType::HistoryRow, r.dB.data(), r.dB.size(),
                r.fft_size,
                static_cast<uint32_t>(start + i),
                r.sample_rate, r.flags);
            state.outputs.history.push_back(std::move(row_buf));
        }
        state.outputs.ready.store(true, std::memory_order_release);
    }
    static void set_history_capacity(AppState& state, const json& j) {
        state.history.set_capacity(j.value("value", std::size_t{4096}));
    }
    static void export_capture(AppState& state, const json& j) {
        // Floats live after the 16-byte FrameHeader in the cached frame
        // bytes - decode in place rather than keep a parallel buffer.
        const std::string format = j.value("format", std::string{});
        const std::string path   = j.value("path",   std::string{});
        const std::string source = j.value("source", std::string{});
        if (path.empty()) {
            std::osyncstream(std::cout) << "[export] empty path, skipping" << std::endl;
            return;
        }
        std::vector<uint8_t> bytes;
        {
            std::lock_guard<std::mutex> lock(state.outputs.mu);
            if      (source == "spectrum") bytes = state.outputs.spec;
            else if (source == "waveform") bytes = state.outputs.wave;
        }
        if (bytes.size() <= sizeof(FrameHeader)) {
            std::osyncstream(std::cout) << "[export] no " << source << " data available yet"
                                        << std::endl;
            return;
        }
        FrameHeader hdr{};
        std::memcpy(&hdr, bytes.data(), sizeof(FrameHeader));
        const std::span<const float> payload(
            reinterpret_cast<const float*>(bytes.data() + sizeof(FrameHeader)),
            (bytes.size() - sizeof(FrameHeader)) / sizeof(float));
        using namespace signalscope::exports;
        bool ok = false;
        if      (format == "csv")  ok = write_csv_spectrum(path, payload, hdr.sample_rate);
        else if (format == "raw")  ok = write_raw_float32(path, payload);
        else if (format == "wav")  ok = write_wav_float32(path, payload, hdr.sample_rate);
        else if (format == "aiff") ok = write_aiff_int16(path, payload, hdr.sample_rate);
        std::osyncstream(std::cout) << "[export] " << format << ' ' << source << " -> " << path
                                    << " (" << payload.size() << " floats) "
                                    << (ok ? "ok" : "FAILED") << std::endl;
    }
    static void set_taper_count(AppState& state, const json& j) {
        std::lock_guard<std::mutex> lock(state.dsp_mutex);
        state.dsp.set_taper_count(j.value("value", 1));
    }
    static void set_overlap(AppState& state, const json& j) {
        // Quantize the percentage to {0, 50, 75, 87.5, 93.75}% ->
        // {1, 2, 4, 8, 16}× overlap factor.
        const int pct = j.value("value", 0);
        int factor = 1;
        if      (pct >= 93) factor = 16;
        else if (pct >= 87) factor = 8;
        else if (pct >= 75) factor = 4;
        else if (pct >= 50) factor = 2;
        std::lock_guard<std::mutex> lock(state.dsp_mutex);
        state.dsp.set_overlap(factor);
    }
    static void set_replay_speed(AppState& state, const json& j) {
        // No mutex — FileSource keeps its own atomic speed and the
        // worker thread reads it on each chunk-pacing tick.
        state.file_source.set_replay_speed(j.value("value", 1.0f));
    }
    static void set_weighting(AppState& state, const json& j) {
        const std::string v = j.value("value", std::string{"none"});
        state.weighting.store(static_cast<int>(weighting_from_string(v)),
                              std::memory_order_release);
    }
    static void set_suppress_image(AppState& state, const json& j) {
        state.suppress_image.store(j.value("value", false),
                                   std::memory_order_release);
        // Drop the Hilbert FIR delay line so stale history doesn't
        // bleed across the toggle. ~32 samples of warm-up follow.
        std::lock_guard<std::mutex> lock(state.dsp_mutex);
        state.hilbert.reset();
    }
    // set_tune_offset and set_decimation share a configure() call -
    // FIR taps depend on M, NCO phase on tune; both must stay coherent.
    static void set_tune_offset(AppState& state, const json& j) {
        std::lock_guard<std::mutex> lock(state.dsp_mutex);
        const float tune = j.value("value", 0.0f);
        state.ddc.configure(tune, state.ddc.decimation(), state.sample_rate);
        state.dsp.clear_peak_hold();
    }
    static void set_decimation(AppState& state, const json& j) {
        std::lock_guard<std::mutex> lock(state.dsp_mutex);
        std::size_t dec = j.value("value", std::size_t{1});
        if (dec < 1) dec = 1;
        state.ddc.configure(state.ddc.tune_hz(), dec, state.sample_rate);
        state.dsp.clear_peak_hold();
    }
    static void set_source(AppState& state, const json& j) {
        std::string new_mode;
        {
            std::lock_guard<std::mutex> lock(state.source_mutex);
            new_mode = j.value("value", std::string{});
            state.source_mode = new_mode;
        }
        // Leaving file mode: tear down the file worker so it stops feeding samples.
        if (new_mode != "file") {
            state.file_source.stop();
            if (new_mode != "listen") {
                state.source_is_complex.store(false, std::memory_order_release);
                state.source_center_freq.store(0.0f, std::memory_order_release);
            }
        }
        if (new_mode != "listen") {
            state.iq_listener.stop();
            return;
        }
        // Entering listen - apply declared params before bind so the
        // renderer's axis updates even before a publisher connects.
        state.source_is_complex.store(
            state.listen_defaults.format != IqListenerSource::Format::F32Real,
            std::memory_order_release);
        state.source_center_freq.store(
            static_cast<float>(state.listen_defaults.center_freq),
            std::memory_order_release);
        {
            std::lock_guard<std::mutex> dlock(state.dsp_mutex);
            const float fr = static_cast<float>(state.listen_defaults.sample_rate);
            if (fr > 0.0f) {
                state.sample_rate = fr;
                state.dsp.set_sample_rate(fr);
                state.ddc.configure(state.ddc.tune_hz(),
                                    state.ddc.decimation(), fr);
            }
        }
        rebind_listener(state, state.listen_port.load());
    }
    static void set_listen_port(AppState& state, const json& j) {
        const uint16_t p = static_cast<uint16_t>(j.value("value", 5555));
        state.listen_port.store(p);
        // Hot port change: only acts if we're currently in listen mode
        // (otherwise we'd be binding a port the user can't see).
        if (current_source_mode(state) == "listen") rebind_listener(state, p);
    }
    static void set_listen_format(AppState& state, const json& j) {
        state.listen_defaults.format = IqListenerSource::format_from_int(
            j.value("value", 0));
        state.iq_listener.update_defaults(state.listen_defaults);
        // Reflect format-implied complex-ness immediately so the renderer
        // adjusts its axis even before a publisher connects.
        if (current_source_mode(state) == "listen") {
            state.source_is_complex.store(
                state.listen_defaults.format != IqListenerSource::Format::F32Real,
                std::memory_order_release);
        }
    }
    static void set_listen_sample_rate(AppState& state, const json& j) {
        state.listen_defaults.sample_rate = j.value("value", std::uint32_t{0});
        state.iq_listener.update_defaults(state.listen_defaults);
        if (current_source_mode(state) == "listen"
            && state.listen_defaults.sample_rate > 0) {
            std::lock_guard<std::mutex> dlock(state.dsp_mutex);
            const float fr = static_cast<float>(state.listen_defaults.sample_rate);
            state.sample_rate = fr;
            state.dsp.set_sample_rate(fr);
            state.ddc.configure(state.ddc.tune_hz(),
                                state.ddc.decimation(), fr);
        }
    }
    static void set_listen_center_freq(AppState& state, const json& j) {
        state.listen_defaults.center_freq = j.value("value", std::uint32_t{0});
        state.iq_listener.update_defaults(state.listen_defaults);
        if (current_source_mode(state) == "listen") {
            state.source_center_freq.store(
                static_cast<float>(state.listen_defaults.center_freq),
                std::memory_order_release);
        }
    }
    static void set_listen_expect_header(AppState& state, const json& j) {
        state.listen_defaults.expect_header = j.value("value", true);
        state.iq_listener.update_defaults(state.listen_defaults);
    }
    static void load_file(AppState& state, const json& j) {
        // Rich payload (flat): { cmd, path, format, datatype?, sample_rate?, channels? }
        LoadFileRequest req;
        req.path        = j.value("path",        std::string{});
        req.format      = j.value("format",      std::string{});
        req.datatype    = j.value("datatype",    std::string{});
        req.sample_rate = j.value("sample_rate", std::uint32_t{0});
        req.channels    = j.value("channels",    std::uint32_t{1});
        if (req.channels == 0) req.channels = 1;
        // Center frequency (Hz). Only meaningful for SDR/IQ captures.
        const float center_hz = j.value("center_frequency", 0.0f);

        // Backwards-compat: older renderers might still send {value:"/path"}.
        if (req.path.empty()) req.path = j.value("value", std::string{});
        if (req.format.empty()) {
            // Infer format from extension as a fallback.
            std::string ext;
            const auto dot = req.path.find_last_of('.');
            if (dot != std::string::npos) ext = req.path.substr(dot + 1);
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c){ return std::tolower(c); });
            if      (ext == "wav")  req.format = "wav";
            else if (ext == "aiff" || ext == "aif") req.format = "aiff";
            else if (ext == "f32")  req.format = "f32";
            else if (ext == "raw")  req.format = "raw";
            else if (ext == "sigmf-data"
                     || req.path.find(".sigmf-data") != std::string::npos)
                                    req.format = "sigmf";
        }

        if (req.path.empty()) {
            std::osyncstream(std::cerr) << "[ws] load_file: missing path\n";
            return;
        }
        state.file_source.stop();
        auto decoder = build_decoder(req);
        if (!decoder) {
            std::osyncstream(std::cerr) << "[ws] load_file: failed to build decoder\n";
            return;
        }
        state.source_is_complex.store(decoder->is_complex(),
                                      std::memory_order_release);
        state.source_center_freq.store(center_hz,
                                       std::memory_order_release);
        // Reset loop counter and ring buffer for a clean start.
        state.source_loop_count.store(0, std::memory_order_relaxed);
        state.capture_buffer.reset();
        {
            // Combined dsp_mutex section: clear peak-hold + switch
            // engine sample rate to the file's natural rate in one go.
            std::lock_guard<std::mutex> dlock(state.dsp_mutex);
            state.dsp.clear_peak_hold();
            const float fr = static_cast<float>(decoder->sample_rate());
            if (fr > 0.0f) {
                state.sample_rate = fr;
                state.dsp.set_sample_rate(fr);
                state.ddc.configure(state.ddc.tune_hz(),
                                    state.ddc.decimation(), fr);
            }
        }
        {
            std::lock_guard<std::mutex> lock(state.source_mutex);
            state.source_file = req.path;
            state.source_mode = "file";
        }
        state.file_source.start(std::move(decoder), state.capture_buffer);
    }

    } // namespace cmd

    using CommandHandler = void(*)(AppState&, const json&);

    // The dispatch table. Keyed by string_view so a command name in the
    // incoming JSON doesn't have to allocate to look itself up. Built
    // once on first call (Meyers singleton - thread-safe init since
    // C++11) and never mutated, so concurrent lookups are race-free.
    static const std::unordered_map<std::string_view, CommandHandler>&
    command_table()
    {
        static const std::unordered_map<std::string_view, CommandHandler> t = {
            {"set_fft_size",             &cmd::set_fft_size},
            {"set_window",               &cmd::set_window},
            {"set_gain",                 &cmd::set_gain},
            {"set_avg_count",            &cmd::set_avg_count},
            {"set_sample_rate",          &cmd::set_sample_rate},
            {"set_pause",                &cmd::set_pause},
            {"set_peak_hold",            &cmd::set_peak_hold},
            {"clear_peak_hold",          &cmd::clear_peak_hold},
            {"set_peak_min_level",       &cmd::set_peak_min_level},
            {"request_history",          &cmd::request_history},
            {"set_history_capacity",     &cmd::set_history_capacity},
            {"export_capture",           &cmd::export_capture},
            {"set_taper_count",          &cmd::set_taper_count},
            {"set_overlap",              &cmd::set_overlap},
            {"set_replay_speed",         &cmd::set_replay_speed},
            {"set_weighting",            &cmd::set_weighting},
            {"set_suppress_image",       &cmd::set_suppress_image},
            {"set_tune_offset",          &cmd::set_tune_offset},
            {"set_decimation",           &cmd::set_decimation},
            {"set_source",               &cmd::set_source},
            {"set_listen_port",          &cmd::set_listen_port},
            {"set_listen_format",        &cmd::set_listen_format},
            {"set_listen_sample_rate",   &cmd::set_listen_sample_rate},
            {"set_listen_center_freq",   &cmd::set_listen_center_freq},
            {"set_listen_expect_header", &cmd::set_listen_expect_header},
            {"load_file",                &cmd::load_file},
        };
        return t;
    }

    static void handle_command(AppState &state, const std::string &msg)
    {
        json j;
        try {
            j = json::parse(msg);
        } catch (const std::exception& e) {
            std::osyncstream(std::cerr) << "[ws] malformed JSON: " << e.what() << '\n';
            return;
        }
        const std::string cmd = j.value("cmd", std::string{});
        if (cmd.empty()) return;

        const auto& table = command_table();
        const auto it = table.find(std::string_view{cmd});
        if (it == table.end()) {
            std::osyncstream(std::cerr) << "[ws] unknown command: " << cmd << '\n';
            return;
        }
        // Per-handler try/catch so a malformed value (e.g. wrong type
        // for a single field) takes down only its own command, not the
        // WS thread.
        try {
            it->second(state, j);
        } catch (const std::exception& e) {
            std::osyncstream(std::cerr) << "[ws] command '" << cmd << "' failed: "
                                        << e.what() << '\n';
        }
    }

    // WebSocket Thread
    // Uses uWebSockets if available, otherwise a minimal WebSocket impl.

#if !defined(USE_SIMPLE_WS)

    struct PerSocketData {};
    using ActiveWS = uWS::WebSocket<false, true, PerSocketData>;

    void ws_thread_func(AppState &state)
    {
        std::osyncstream(std::cout) << "[ws] Thread started on port " << state.ws_port
                                    << std::endl;

        // Track live sockets for direct delivery - no pub/sub subscription needed.
        // All uWS callbacks and the timer fire on the same event-loop thread, so
        // no mutex is required for this vector.
        std::vector<ActiveWS *> clients;

        struct TimerData {
            AppState *state;
            std::vector<ActiveWS *> *clients;
            us_listen_socket_t *listen_socket;  // populated after .listen() succeeds
        };

        us_listen_socket_t *listen_socket_ref = nullptr;

        auto app = uWS::App()
            .ws<PerSocketData>("/*", {
                .compression          = uWS::DISABLED,
                .maxPayloadLength     = 16 * 1024 * 1024,
                .idleTimeout          = 0,      // disable - server streams continuously
                .maxBackpressure      = 4 * 1024 * 1024,
                .closeOnBackpressureLimit = false,
                .resetIdleTimeoutOnSend   = true,
                .sendPingsAutomatically   = false, // not needed with idleTimeout=0

                .open = [&clients](ActiveWS *ws) noexcept {
                    std::osyncstream(std::cout) << "[ws] Client connected" << std::endl;
                    try { clients.push_back(ws); } catch (...) {}
                },

                .message = [&state](auto *, std::string_view message, uWS::OpCode opCode) noexcept {
                    if (opCode == uWS::OpCode::TEXT) {
                        try { handle_command(state, std::string(message)); } catch (...) {}
                    }
                },

                .close = [&clients, &state](ActiveWS *ws, int code, std::string_view) noexcept {
                    // Suppress the log entirely while shutting down: state.running is
                    // already false because the signal handler / Electron-side stopBackend
                    // tripped it, so every client disconnect at this point is expected
                    // (we explicitly call ws->close() on each one below). Logging them as
                    // [backend:err] on stderr surfaces them in Electron's child-stderr pipe
                    // and looks like a real error in the developer's terminal.
                    //
                    // A renderer crash *before* SIGTERM still logs (to stdout below) -
                    // intentional, so genuine crashes stay observable.
                    if (state.running.load(std::memory_order_relaxed)) {
                        // Use stdout to match the [ws] Client connected log channel.
                        std::osyncstream(std::cout) << "[ws] Client disconnected (" << code << ")\n";
                    }
                    auto it = std::find(clients.begin(), clients.end(), ws);
                    if (it != clients.end()) clients.erase(it);
                }
            })
            .listen(state.ws_port, [&state, &listen_socket_ref](auto *listen_socket) {
                if (!listen_socket) {
                    std::osyncstream(std::cerr) << "[ws] fatal: listen failed on port "
                                                << state.ws_port
                                                << " (already in use or permission denied)"
                                                << std::endl;
                    state.ws_fatal.store(true, std::memory_order_release);
                    return;
                }
                listen_socket_ref = listen_socket;
                std::osyncstream(std::cout) << "[ss] Listening on port " << state.ws_port << std::endl;
            });

        if (state.ws_fatal.load(std::memory_order_acquire)) {
            return;  // bail out of ws_thread_func; main thread will join.
        }

        // Timer: send frames directly to every connected socket at ~60 fps.
        auto *loop = reinterpret_cast<us_loop_t *>(uWS::Loop::get());
        auto *timer = us_create_timer(loop, 0, sizeof(TimerData));
        auto *td = static_cast<TimerData *>(us_timer_ext(timer));
        td->state         = &state;
        td->clients       = &clients;
        td->listen_socket = listen_socket_ref;

        us_timer_set(timer, [](us_timer_t *t) {
            try {
                auto *td = static_cast<TimerData *>(us_timer_ext(t));
                AppState *s = td->state;

                // Graceful shutdown: when state.running goes false (signal handler
                // set it), close everything so app.run() returns. Without this, the
                // uWS loop has no path to exit on SIGTERM and the process can only
                // die via the parent's SIGKILL fallback (3 s in Electron's main.js
                // stopBackend) — see docs/superpowers/plans/2026-05-14-backend-
                // lifecycle-fixes.md for the broader picture.
                if (!s->running.load(std::memory_order_relaxed)) {
                    // .close callbacks mutate *clients, so iterate over a snapshot.
                    auto clients_snapshot = *td->clients;
                    // Best-effort: a throw from any single close() must not stop the
                    // rest of the shutdown sequence.
                    for (auto *ws : clients_snapshot) {
                        try { ws->close(); } catch (...) {}
                    }
                    if (td->listen_socket) {
                        us_listen_socket_close(/*ssl=*/0, td->listen_socket);
                        td->listen_socket = nullptr;
                    }
                    us_timer_close(t);
                    return;
                }

                if (td->clients->empty()) return;
                if (!s->outputs.ready.load(std::memory_order_acquire)) return;

                std::vector<uint8_t> spec, hold, wave, phase, hist, iq, status, stats;
                std::vector<std::vector<uint8_t>> history;
                {
                    std::lock_guard<std::mutex> lock(s->outputs.mu);
                    // Swap rather than copy. The WS-thread locals retain the
                    // DSP-thread buffers we just claimed; on the next DSP
                    // cycle, the DSP thread writes into the buffers we hand
                    // back (already sized from last time), so steady-state
                    // allocations on this path are zero.
                    std::swap(spec,   s->outputs.spec);
                    std::swap(hold,   s->outputs.hold);
                    std::swap(wave,   s->outputs.wave);
                    std::swap(phase,  s->outputs.phase);
                    std::swap(hist,   s->outputs.hist);
                    std::swap(iq,     s->outputs.iq);
                    std::swap(status, s->outputs.status);
                    std::swap(stats,  s->outputs.stats);
                    history = std::move(s->outputs.history);
                    s->outputs.history.clear();
                    s->outputs.ready.store(false, std::memory_order_release);
                }

                for (auto *ws : *td->clients) {
                    auto send_frame = [&](const std::vector<uint8_t> &data) -> bool {
                        if (data.empty()) return true; // skip empty (hold off, etc.)
                        auto sv = std::string_view(
                            reinterpret_cast<const char *>(data.data()), data.size());
                        return ws->send(sv, uWS::OpCode::BINARY, false)
                               != ActiveWS::SendStatus::BACKPRESSURE;
                    };
                    // Stop sending to this client if the socket buffer is full
                    if (!send_frame(spec))   continue;
                    if (!send_frame(hold))   continue;
                    if (!send_frame(stats))  continue;
                    if (!send_frame(wave))   continue;
                    if (!send_frame(phase))  continue;
                    if (!send_frame(hist))   continue;
                    if (!send_frame(iq))     continue;
                    if (!send_frame(status)) continue;
                    // History rows last so a backpressure stall doesn't
                    // block the live frames. Each row is its own frame.
                    bool ok = true;
                    for (const auto& row : history) {
                        if (!send_frame(row)) { ok = false; break; }
                    }
                    if (!ok) continue;
                }

                // Recycle the row buffers we just sent. They go back under
                // the same mutex that the request_history handler will pull
                // from on its next invocation.
                if (!history.empty()) {
                    std::lock_guard<std::mutex> lock(s->outputs.mu);
                    // Cap the pool. historyCapacity is 4096 rows by default;
                    // 64 retired buffers handles every normal request size
                    // without retaining the worst-case batch forever.
                    constexpr std::size_t kHistoryPoolMax = 64;
                    for (auto& row : history) {
                        if (s->outputs.history_pool.size() < kHistoryPoolMax) {
                            s->outputs.history_pool.push_back(std::move(row));
                        }
                    }
                    history.clear();
                }
            } catch (...) {}
        }, 16, 16); // ~60 fps

        app.run();
        std::osyncstream(std::cout) << "[ws] Thread stopped" << std::endl;
    }

#else
    // Fallback: Simple WebSocket server (no uWebSockets dep)
    // POSIX sockets + a poll() event loop. Pure WebSocket framing
    // (parse_ws_frame / base64_encode / build_ws_binary_header) lives in
    // protocol/ws_framing.hpp so the test target can exercise it.

    // Per-client state for the simple-WS fallback. Each connection keeps an
    // accumulating inbox so partial reads (TCP doesn't preserve message
    // boundaries) are handled correctly.
    struct WSClient {
        int         fd;
        bool        handshaked;
        std::string inbox;
    };

    // Send a server->client binary WS frame. Returns false on any send
    // error (including EAGAIN backpressure) - caller drops the client.
    static bool ws_send_binary(int fd, const uint8_t *data, size_t len)
    {
        uint8_t header[10];
        const size_t hlen = build_ws_binary_header(header, len);
        if (send(fd, header, hlen, MSG_NOSIGNAL) < 0) return false;
        if (send(fd, data,   len,  MSG_NOSIGNAL) < 0) return false;
        return true;
    }

    // Multi-client simple-WS server. Single-threaded poll() event loop:
    //   - listen socket woken on POLLIN -> accept all pending connections
    //   - each connected fd woken on POLLIN -> drain into per-client inbox,
    //       run the HTTP upgrade once, then parse zero or more WS frames
    //   - every poll wake-up checks `outputs.ready` and broadcasts to all
    //       handshaked clients
    // No per-client thread; backpressure on one client doesn't stall others
    // because both reads and writes use MSG_DONTWAIT.
    void ws_thread_func(AppState &state)
    {
        std::osyncstream(std::cout) << "[ws] Simple WebSocket server starting on port "
                                    << state.ws_port << std::endl;

        // Small helper: stringify errno the std::cerr way, since perror()
        // doesn't compose with the iostreams pipeline we use elsewhere.
        auto log_syscall_err = [](const char* what) {
            std::osyncstream(std::cerr) << what << ": " << std::strerror(errno) << '\n';
        };

        int server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd < 0) { log_syscall_err("[ws] socket"); return; }

        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(state.ws_port);
        if (bind(server_fd, (sockaddr *)&addr, sizeof(addr)) < 0) {
            std::osyncstream(std::cerr) << "[ws] fatal: bind failed on port " << state.ws_port
                                        << " (" << std::strerror(errno) << ")" << std::endl;
            state.ws_fatal.store(true, std::memory_order_release);
            close(server_fd);
            return;
        }
        if (listen(server_fd, 16) < 0) {
            std::osyncstream(std::cerr) << "[ws] fatal: listen failed on port " << state.ws_port
                                        << " (" << std::strerror(errno) << ")" << std::endl;
            state.ws_fatal.store(true, std::memory_order_release);
            close(server_fd);
            return;
        }
        // Non-blocking listen so accept() returns immediately when nothing
        // is pending; non-blocking client fds so a slow consumer can't stall
        // the broadcast loop.
        fcntl(server_fd, F_SETFL, O_NONBLOCK);

        std::osyncstream(std::cout) << "[ws] Listening on port " << state.ws_port << std::endl;

        std::vector<WSClient> clients;
        std::vector<pollfd>   pfds;

        while (state.running.load())
        {
            // Rebuild poll set each iteration - simplest correct way given
            // accept/drop within the same iteration. Listen socket is always
            // first so we know its index.
            pfds.clear();
            pfds.reserve(clients.size() + 1);
            pfds.push_back({server_fd, POLLIN, 0});
            for (auto &c : clients) pfds.push_back({c.fd, POLLIN, 0});

            const int prc = poll(pfds.data(), pfds.size(), 5);
            if (prc < 0) {
                if (errno == EINTR) continue;
                log_syscall_err("[ws] poll");
                break;
            }

            // Per-client receive FIRST
            // Process before accept() so the index-by-index pairing of
            // pfds and clients stays valid. New connections land in pfds
            // on the next outer iteration; they have nothing buffered to
            // drain yet anyway.
            for (size_t i = 0; i < clients.size(); ) {
                auto &c = clients[i];
                const auto &pfd = pfds[i + 1];
                bool drop = false;

                if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
                    drop = true;
                } else if (pfd.revents & POLLIN) {
                    // Drain socket into inbox (multiple recvs in case the
                    // client sent more than 8 KB before we got here).
                    uint8_t buf[8192];
                    for (;;) {
                        ssize_t nr = recv(c.fd, buf, sizeof(buf), MSG_DONTWAIT);
                        if (nr > 0) {
                            c.inbox.append(reinterpret_cast<char *>(buf),
                                           static_cast<size_t>(nr));
                        } else if (nr == 0) {
                            drop = true; break;
                        } else {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                            drop = true; break;
                        }
                    }

// HTTP upgrade (one-shot per connection)
if (!drop && !c.handshaked) {
                        const auto end = c.inbox.find("\r\n\r\n");
                        if (end != std::string::npos) {
                            const auto kp = c.inbox.find("Sec-WebSocket-Key: ");
                            if (kp == std::string::npos || kp > end) {
                                drop = true;
                            } else {
                                const auto ks = kp + 19;
                                const auto ke = c.inbox.find("\r\n", ks);
                                std::string ws_key = c.inbox.substr(ks, ke - ks);
                                std::string accept_in = ws_key +
                                    "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
                                unsigned char sha1_hash[20];
                                SHA1(reinterpret_cast<const unsigned char *>(
                                        accept_in.c_str()),
                                     accept_in.length(), sha1_hash);
                                std::string accept_v = base64_encode(sha1_hash, 20);
                                std::string resp =
                                    "HTTP/1.1 101 Switching Protocols\r\n"
                                    "Upgrade: websocket\r\n"
                                    "Connection: Upgrade\r\n"
                                    "Sec-WebSocket-Accept: " + accept_v + "\r\n\r\n";
                                send(c.fd, resp.c_str(), resp.length(), MSG_NOSIGNAL);
                                c.handshaked = true;
                                c.inbox.erase(0, end + 4);
                            }
                        }
                    }

// WS frame parsing - zero or more per read
while (!drop && c.handshaked && !c.inbox.empty()) {
                        uint8_t op;
                        std::string payload;
                        const int consumed = parse_ws_frame(
                            reinterpret_cast<const uint8_t *>(c.inbox.data()),
                            c.inbox.size(), op, payload);
                        if (consumed < 0) break;          // need more bytes
                        if (consumed == 0) { drop = true; break; }
                        if (op == 0x1) {                  // text
                            try { handle_command(state, payload); } catch (...) {}
                        } else if (op == 0x8) {           // close
                            drop = true;
                        }
                        // ping (0x9) / pong (0xA) ignored
                        c.inbox.erase(0, static_cast<size_t>(consumed));
                    }
                }

                if (drop) {
                    close(c.fd);
                    clients.erase(clients.begin() + i);
                    // Mirror the uWS .close handler's shutdown-quiet guard (see the
                    // stderr-channel rationale there). simple-WS already logs to stdout
                    // so this is purely a noise reduction for the dev terminal, not a
                    // stream-routing fix.
                    if (state.running.load(std::memory_order_relaxed)) {
                        std::osyncstream(std::cout) << "[ws] Client disconnected (" << clients.size()
                                                    << " total)" << std::endl;
                    }
                } else {
                    ++i;
                }
            }

            // Accept new connections AFTER per-client receive so new
            // fds don't get index-misaligned with pfds. They sit
            // unhandshaked until the next poll picks up their GET.
            if (pfds[0].revents & POLLIN) {
                for (;;) {
                    int fd = accept(server_fd, nullptr, nullptr);
                    if (fd < 0) break;
                    fcntl(fd, F_SETFL, O_NONBLOCK);
                    clients.push_back({fd, /*handshaked*/ false, std::string{}});
                    std::osyncstream(std::cout) << "[ws] Client connected (" << clients.size()
                                                << " total)" << std::endl;
                }
            }

// Broadcast to all handshaked clients
if (!clients.empty() &&
                state.outputs.ready.load(std::memory_order_acquire))
            {
                std::vector<uint8_t> spec, hold, wave, phase, hist, iq, status, stats;
                std::vector<std::vector<uint8_t>> history;
                {
                    std::lock_guard<std::mutex> lock(state.outputs.mu);
                    // Swap rather than copy. The WS-thread locals retain the
                    // DSP-thread buffers we just claimed; on the next DSP
                    // cycle, the DSP thread writes into the buffers we hand
                    // back (already sized from last time), so steady-state
                    // allocations on this path are zero.
                    std::swap(spec,   state.outputs.spec);
                    std::swap(hold,   state.outputs.hold);
                    std::swap(wave,   state.outputs.wave);
                    std::swap(phase,  state.outputs.phase);
                    std::swap(hist,   state.outputs.hist);
                    std::swap(iq,     state.outputs.iq);
                    std::swap(status, state.outputs.status);
                    std::swap(stats,  state.outputs.stats);
                    history = std::move(state.outputs.history);
                    state.outputs.history.clear();
                    state.outputs.ready.store(false, std::memory_order_release);
                }
                for (size_t i = 0; i < clients.size(); ) {
                    auto &c = clients[i];
                    if (!c.handshaked) { ++i; continue; }
                    auto send_one = [&](const std::vector<uint8_t> &d) {
                        if (d.empty()) return true;
                        return ws_send_binary(c.fd, d.data(), d.size());
                    };
                    // Same order the uWS path uses. Any failure (including
                    // EAGAIN backpressure) drops the client - they'll
                    // reconnect via the renderer's 300ms retry.
                    bool ok = send_one(spec)  && send_one(hold)  && send_one(stats)
                           && send_one(wave)  && send_one(phase) && send_one(hist)
                           && send_one(iq)    && send_one(status);
                    if (ok) {
                        for (const auto& row : history) {
                            if (!send_one(row)) { ok = false; break; }
                        }
                    }
                    if (!ok) {
                        close(c.fd);
                        clients.erase(clients.begin() + i);
                        std::osyncstream(std::cout) << "[ws] Client send failed, dropped ("
                                                    << clients.size() << " total)" << std::endl;
                    } else {
                        ++i;
                    }
                }

                // Recycle the row buffers we just sent. They go back under
                // the same mutex that the request_history handler will pull
                // from on its next invocation.
                if (!history.empty()) {
                    std::lock_guard<std::mutex> lock(state.outputs.mu);
                    // Cap the pool. historyCapacity is 4096 rows by default;
                    // 64 retired buffers handles every normal request size
                    // without retaining the worst-case batch forever.
                    constexpr std::size_t kHistoryPoolMax = 64;
                    for (auto& row : history) {
                        if (state.outputs.history_pool.size() < kHistoryPoolMax) {
                            state.outputs.history_pool.push_back(std::move(row));
                        }
                    }
                    history.clear();
                }
            }
        }

        for (auto &c : clients) close(c.fd);
        close(server_fd);
        std::osyncstream(std::cout) << "[ws] Server stopped" << std::endl;
    }

#endif // USE_SIMPLE_WS

} // namespace signalscope

static signalscope::AppState *g_state = nullptr;

static void signal_handler(int sig)
{
    // std::cout isn't async-signal-safe; printf is the conventional
    // shutdown-handler choice (glibc treats it as safe for line writes
    // even though POSIX doesn't formally guarantee it).
    std::printf("\n[ss] Received signal %d, shutting down...\n", sig);
    if (g_state) g_state->running.store(false);
}

int main(int argc, char *argv[])
{
    // FFT backend selection must happen BEFORE AppState constructs the
    // DSPPipeline, since the pipeline's constructor calls make_backend().
    // Resolution order: --fft-backend= flag > SS_FFT_BACKEND env > default.
    std::string fft_pref;
    for (int i = 1; i < argc; i++) {
        std::string arg(argv[i]);
        if (arg.starts_with("--fft-backend=")) {
            fft_pref = arg.substr(std::string_view("--fft-backend=").size());
            break;
        }
    }
    ss::fft::initialize(fft_pref);
    ss::fft::global_startup();

    signalscope::AppState state;
    g_state = &state;

    // Parse command line
    for (int i = 1; i < argc; i++)
    {
        std::string arg(argv[i]);
        if (arg.starts_with("--port="))
        {
            state.ws_port = std::stoi(arg.substr(7));
        }
        else if (arg == "--test")
        {
            std::lock_guard<std::mutex> lock(state.source_mutex);
            state.source_mode = "test";
        }
        else if (arg == "--mic")
        {
            std::lock_guard<std::mutex> lock(state.source_mutex);
            state.source_mode = "mic";
        }
        else if (arg.starts_with("--fft="))
        {
            state.dsp.set_fft_size(std::stoul(arg.substr(6)));
        }
        else if (arg.starts_with("--rate="))
        {
            state.sample_rate = std::stof(arg.substr(7));
            state.dsp.set_sample_rate(state.sample_rate);
        }
        else if (arg.starts_with("--fft-backend="))
        {
            // Already consumed by the pre-pass before AppState was built.
        }
        else if (arg == "--list-devices")
        {
            auto devices = signalscope::AudioCapture::enumerate_devices();
            std::cout << "Capture devices:\n";
            for (const auto &d : devices)
            {
                std::cout << "  [" << d.id_index << "] " << d.name
                          << (d.is_default ? " (default)" : "") << '\n';
            }
            return 0;
        }
        else if (arg == "--help" || arg == "-h")
        {
            std::cout
                << "signalscope-backend [options]\n\n"
                << "  --port=N           WebSocket port (default: 8765)\n"
                << "  --test             Use test signal generator\n"
                << "  --mic              Use microphone capture\n"
                << "  --fft=N            FFT size (default: 4096)\n"
                << "  --rate=N           Sample rate (default: 48000)\n"
                << "  --fft-backend=NAME FFT backend: pocketfft (default), fftw, kfr.\n"
                << "                     SS_FFT_BACKEND env var also accepted.\n"
                << "  --list-devices     List audio capture devices\n"
                << "  -h, --help         Show this help\n";
            return 0;
        }
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "SignalScope Backend v0.2.0\n"
              << "  FFT size:    " << state.dsp.config().fft_size << '\n'
              << "  FFT backend: " << ss::fft::active_name() << '\n'
              << "  Sample rate: " << state.sample_rate << " Hz\n"
              << "  Source:      " << state.source_mode << '\n'
              << "  WS port:     " << state.ws_port << std::endl;

    // Start microphone capture if requested
    if (state.source_mode == "mic")
    {
        signalscope::AudioCapture::Config acfg;
        acfg.sample_rate = static_cast<uint32_t>(state.sample_rate);
        acfg.channels = 1;
        acfg.buffer_frames = 1024;

        bool ok = state.audio.start(acfg, [&state](const float *data, size_t count) {
            // Ring auto-drops oldest on overflow; the audio callback
            // has no useful response to backpressure.
            std::ignore = state.capture_buffer.write(data, count);
        });

        if (!ok)
        {
            std::cerr << "[main] Falling back to test signal\n";
            std::lock_guard<std::mutex> lock(state.source_mutex);
            state.source_mode = "test";
        }
    }

    std::thread dsp(signalscope::dsp_thread_func, std::ref(state));

    // Background backend warmup. Pocketfft / KFR are no-ops; FFTW
    // populates wisdom for the common sizes so set_fft_size never
    // freezes the UI on a cold machine.
    signalscope::g_warmup_running = &state.running;
    std::thread fft_warmup(signalscope::fft_warmup_thread);

    // WS server runs on the main thread (owns its own event loop).
    // ws_thread_func() returns early if state.ws_fatal was set (bind/listen
    // failure on either the uWS or simple-WS path). Detect that here so we
    // log the fatal-shutdown reason before the normal teardown runs.
    signalscope::ws_thread_func(state);

    if (state.ws_fatal.load(std::memory_order_acquire)) {
        std::osyncstream(std::cerr) << "[main] shutting down due to ws_fatal" << std::endl;
    }

    state.running.store(false);
    dsp.join();
    if (fft_warmup.joinable()) fft_warmup.join();
    state.audio.stop();

    ss::fft::global_shutdown();

    std::cout << "SignalScope Backend shutdown complete" << std::endl;
    return 0;
}