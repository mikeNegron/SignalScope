#pragma once
#include <cmath>
#include <numbers>
#include <algorithm>
#include <functional>
#include <iostream>
#include <string>
#include <vector>
#include <stdexcept>
#include <random>

// miniaudio config - we only need capture, not playback
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#include "third_party/miniaudio.h"

namespace signalscope
{

    struct AudioDevice
    {
        std::string name;
        uint32_t id_index;
        bool is_default;
    };

    class AudioCapture
    {
    public:
        using Callback = std::function<void(const float *, size_t)>;

        struct Config
        {
            uint32_t sample_rate = 48000;
            uint32_t channels = 1;         // 1=mono, 2=stereo
            uint32_t buffer_frames = 1024; // frames per callback
            int device_index = -1;         // -1 = default device
        };

        AudioCapture() = default;
        ~AudioCapture() { stop(); }

        // Non-copyable
        AudioCapture(const AudioCapture &) = delete;
        AudioCapture &operator=(const AudioCapture &) = delete;

        /// List available capture devices
        static std::vector<AudioDevice> enumerate_devices()
        {
            std::vector<AudioDevice> result;
            ma_context context;
            if (ma_context_init(nullptr, 0, nullptr, &context) != MA_SUCCESS)
            {
                return result;
            }

            ma_device_info *capture_infos;
            ma_uint32 capture_count;
            if (ma_context_get_devices(&context, nullptr, nullptr,
                                       &capture_infos, &capture_count) == MA_SUCCESS)
            {
                for (ma_uint32 i = 0; i < capture_count; i++)
                {
                    result.push_back({
                        .name = capture_infos[i].name,
                        .id_index = i,
                        .is_default = capture_infos[i].isDefault != 0,
                    });
                }
            }

            ma_context_uninit(&context);
            return result;
        }

        /// Start capturing audio with the given callback
        bool start(const Config &cfg, Callback cb)
        {
            config_ = cfg;
            callback_ = std::move(cb);

            ma_device_config dev_config = ma_device_config_init(ma_device_type_capture);
            dev_config.capture.format = ma_format_f32;
            dev_config.capture.channels = cfg.channels;
            dev_config.sampleRate = cfg.sample_rate;
            dev_config.periodSizeInFrames = cfg.buffer_frames;
            dev_config.dataCallback = data_callback;
            dev_config.pUserData = this;

            if (ma_device_init(nullptr, &dev_config, &device_) != MA_SUCCESS)
            {
                std::cerr << "[audio] Failed to init capture device\n";
                return false;
            }

            if (ma_device_start(&device_) != MA_SUCCESS)
            {
                std::cerr << "[audio] Failed to start capture device\n";
                ma_device_uninit(&device_);
                return false;
            }

            running_ = true;
            std::cout << "[audio] Capture started: " << device_.capture.name
                      << ", " << cfg.sample_rate << " Hz, "
                      << cfg.channels << " ch" << std::endl;
            return true;
        }

        /// Stop capturing
        void stop()
        {
            if (running_)
            {
                ma_device_stop(&device_);
                ma_device_uninit(&device_);
                running_ = false;
                std::cout << "[audio] Capture stopped" << std::endl;
            }
        }

        bool is_running() const { return running_; }
        const Config &config() const { return config_; }

        /// Generate a test tone for development without audio hardware
        // Generates a synthetic showcase signal with three components
        // that exercise different time/frequency scales - useful for
        // checking that the spectrogram's time-axis zoom + auto-overlap
        // is actually delivering the expected resolution:
        //
        //   1) Slow tone at `freq` (the caller's drifting frequency).
        //      Appears as a slowly-wandering horizontal line - verifies
        //      that frequency resolution stays correct under zoom.
        //
        //   2) Linear chirp 2 kHz -> 8 kHz, sawtooth-resetting every 2 s.
        //      Appears as a clean diagonal on the spectrogram. With low
        //      time resolution the diagonal looks stair-stepped; under
        //      zoom + auto-overlap it stays smooth.
        //
        //   3) Gated 12 kHz pulses, 50 ms on / 150 ms off. Discrete
        //      dashes on the spectrogram - at low zoom they look like a
        //      dotted line, zoomed in they resolve into separate pulses.
        //      Verifies time-axis resolution.
        //
        // Plus a Gaussian noise floor at `noise_floor_dbfs`.
        void generate_test_signal(
            float *buffer,
            size_t frames,
            float freq = 440.0f,
            float noise_floor_dbfs = -60.0f)
        {
            if (!buffer || frames == 0)
            {
                return;
            }

            const double sample_rate = static_cast<double>(config_.sample_rate);
            if (sample_rate <= 0.0)
            {
                std::fill(buffer, buffer + frames, 0.0f);
                return;
            }

            const double two_pi = 2.0 * std::numbers::pi_v<double>;
            const double inv_sr = 1.0 / sample_rate;
            const double tone_inc  = two_pi * static_cast<double>(freq) * inv_sr;
            const double pulse_inc = two_pi * 12000.0 * inv_sr;

            // Chirp params - kept inside the audible range so this works
            // identically whether the device runs at 48 kHz or higher.
            constexpr double CHIRP_LO       = 2000.0;
            constexpr double CHIRP_HI       = 8000.0;
            constexpr double CHIRP_PERIOD_S = 2.0;
            // Pulse gating period. RAMP_S is the raised-cosine fade
            // applied at each edge: a hard 0->A switch on a sinusoid
            // splatters energy across the whole spectrum (the discontinuity
            // is broadband), which would lift the apparent noise floor in
            // sync with the gate. A 5 ms cosine ramp confines the
            // switching energy to a narrow band around 12 kHz.
            constexpr double PULSE_PERIOD_S = 0.20;
            constexpr double PULSE_ON_S     = 0.05;
            constexpr double RAMP_S         = 0.005;

            const double noise_rms = std::pow(10.0,
                static_cast<double>(noise_floor_dbfs) / 20.0);
            std::normal_distribution<double> noise_dist(0.0, noise_rms);

            for (size_t i = 0; i < frames; ++i)
            {
                // Absolute time of this sample within the test signal -
                // wraps modulo each feature's period so we never lose
                // floating-point precision over long runs.
                const double t = static_cast<double>(test_sample_count_) * inv_sr;
                const double t_chirp = std::fmod(t, CHIRP_PERIOD_S);
                const double t_pulse = std::fmod(t, PULSE_PERIOD_S);

                // Linear chirp: sweep frequency linearly from CHIRP_LO to
                // CHIRP_HI over CHIRP_PERIOD_S, then jump back. We
                // accumulate phase from the instantaneous frequency so
                // the trace is phase-continuous within each ramp.
                const double chirp_freq = CHIRP_LO
                    + (CHIRP_HI - CHIRP_LO) * (t_chirp / CHIRP_PERIOD_S);
                test_phase_chirp_ += two_pi * chirp_freq * inv_sr;
                if (test_phase_chirp_ >= two_pi)
                    test_phase_chirp_ = std::fmod(test_phase_chirp_, two_pi);

                // Pulse gate with raised-cosine fades at the edges to
                // suppress switching splatter. Envelope shape per pulse:
                //   ramp up [0, RAMP_S)        : 0.5·(1 - cos(πt/RAMP))
                //   plateau [RAMP_S, ON-RAMP)  : 1
                //   ramp down [ON-RAMP, ON)    : 0.5·(1 - cos(π(ON-t)/RAMP))
                //   off [ON, PERIOD)           : 0
                double env = 0.0;
                if (t_pulse < PULSE_ON_S) {
                    if (t_pulse < RAMP_S) {
                        env = 0.5 * (1.0 - std::cos(std::numbers::pi_v<double> * t_pulse / RAMP_S));
                    } else if (t_pulse > PULSE_ON_S - RAMP_S) {
                        const double fade_t = PULSE_ON_S - t_pulse;
                        env = 0.5 * (1.0 - std::cos(std::numbers::pi_v<double> * fade_t / RAMP_S));
                    } else {
                        env = 1.0;
                    }
                }
                const double pulse_amp = 0.30 * env;

                const float signal = static_cast<float>(
                    0.30 * std::sin(test_phase_tone_)
                  + 0.25 * std::sin(test_phase_chirp_)
                  + pulse_amp * std::sin(test_phase_pulse_));

                const float noise = static_cast<float>(noise_dist(test_noise_rng_));
                buffer[i] = std::clamp(signal + noise, -1.0f, 1.0f);

                test_phase_tone_  += tone_inc;
                test_phase_pulse_ += pulse_inc;
                if (test_phase_tone_  >= two_pi) test_phase_tone_  = std::fmod(test_phase_tone_,  two_pi);
                if (test_phase_pulse_ >= two_pi) test_phase_pulse_ = std::fmod(test_phase_pulse_, two_pi);

                test_sample_count_++;
            }
        }

    private:
        ma_device device_{};
        Config config_{};
        Callback callback_;
        bool running_ = false;
        // Per-component phase accumulators for generate_test_signal.
        // test_sample_count_ is the absolute sample index within the
        // synthetic stream - used to derive periodic feature timing
        // (chirp ramp, pulse gating).
        double   test_phase_tone_   = 0.0;
        double   test_phase_chirp_  = 0.0;
        double   test_phase_pulse_  = 0.0;
        uint64_t test_sample_count_ = 0;
        std::mt19937 test_noise_rng_{std::random_device{}()};

        static void data_callback(ma_device *device, [[maybe_unused]] void *output,
                                  const void *input, ma_uint32 frame_count)
        {
            auto *self = static_cast<AudioCapture *>(device->pUserData);
            if (self->callback_)
            {
                const auto *samples = static_cast<const float *>(input);
                // If stereo, we get interleaved samples - total floats = frame_count * channels
                // For now, pass all samples and let the DSP pipeline handle channel separation
                self->callback_(samples, static_cast<size_t>(frame_count) * device->capture.channels);
            }
        }
    };

} // namespace signalscope
