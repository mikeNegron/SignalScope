#pragma once
// FileSource - owns a worker thread that decodes a file and pumps real or
// interleaved-IQ float samples into the AppState's capture ring buffer at the
// file's natural sample rate.
// Loops on EOF (the user-confirmed behaviour). A monotonic loop counter is
// exposed so the UI can show a non-intrusive "looped N times" indicator.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "audio/file_decoders.hpp"
#include "util/ring_buffer.hpp"

namespace signalscope {

class FileSource {
public:
    FileSource() = default;
    ~FileSource() { stop(); }

    FileSource(const FileSource&) = delete;
    FileSource& operator=(const FileSource&) = delete;

    // Start streaming the file. `decoder` must be already opened. Buffer is
    // the AppState's capture ring (interleaved-IQ when complex, real when not).
    bool start(std::unique_ptr<file_decoder::IDecoder> decoder,
               SPSCRingBuffer<float>& capture_buffer)
    {
        stop();
        if (!decoder) return false;
        decoder_ = std::move(decoder);
        capture_buffer_ = &capture_buffer;
        running_.store(true, std::memory_order_release);
        loop_count_.store(0, std::memory_order_relaxed);
        thread_ = std::thread([this] { worker(); });
        return true;
    }

    void stop() {
        running_.store(false, std::memory_order_release);
        if (thread_.joinable()) thread_.join();
        decoder_.reset();
        capture_buffer_ = nullptr;
    }

    bool is_running() const { return running_.load(std::memory_order_acquire); }

    uint32_t sample_rate() const { return decoder_ ? decoder_->sample_rate() : 0; }
    bool is_complex() const { return decoder_ ? decoder_->is_complex() : false; }
    uint32_t channels() const { return decoder_ ? decoder_->channels() : 0; }
    size_t loop_count() const { return loop_count_.load(std::memory_order_relaxed); }

    // Playback speed multiplier. 1.0 = real-time (the file's natural rate).
    // < 1.0 is slow-mo, > 1.0 is fast-forward. Picked up by the worker on
    // the next chunk-pacing cycle. High speeds may overflow the capture
    // ring (drop-oldest), at which point samples are skipped — acceptable
    // for fast scrub, but the user loses analysis fidelity. Clamped to
    // [0.1, 32.0]. Effective immediately on existing playback.
    void set_replay_speed(float speed) {
        if (!std::isfinite(speed)) speed = 1.0f;
        replay_speed_.store(std::clamp(speed, 0.1f, 32.0f),
                            std::memory_order_relaxed);
    }
    float replay_speed() const {
        return replay_speed_.load(std::memory_order_relaxed);
    }

private:
    void worker() {
        std::cout << "[file] Worker started (sr=" << decoder_->sample_rate()
                  << ", complex=" << decoder_->is_complex()
                  << ", channels=" << decoder_->channels() << ")"
                  << std::endl;

        const size_t fpf = decoder_->floats_per_frame();
        const uint32_t sr = std::max<uint32_t>(decoder_->sample_rate(), 1);

        // Aim to deliver ~50 ms of audio per chunk. That's small enough to
        // keep latency low yet big enough to amortise file-read overhead.
        const size_t chunk_frames = std::max<size_t>(sr / 20, 256);

        std::vector<float> chunk;
        chunk.reserve(chunk_frames * fpf);

        auto next_deadline = std::chrono::steady_clock::now();

        while (running_.load(std::memory_order_relaxed)) {
            chunk.clear();
            size_t got = decoder_->read_frames(chunk, chunk_frames);

            if (got == 0) {
                // EOF - loop and continue.
                decoder_->rewind();
                loop_count_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            if (capture_buffer_) {
                // Drop oldest on overflow is the ring's documented
                // policy; we can't push back on the file producer
                // anyway, so the return value is intentionally ignored.
                std::ignore = capture_buffer_->write(chunk.data(), chunk.size());
            }

            // Pace the producer to the file's natural sample rate, scaled
            // by the user-set replay speed. speed=1 → real-time; speed=10
            // → ten times faster; the chunk duration shrinks accordingly.
            const float speed = std::max(
                0.001f, replay_speed_.load(std::memory_order_relaxed));
            const auto chunk_dur = std::chrono::microseconds(
                static_cast<long long>(static_cast<double>(got) * 1000000.0 /
                                       (static_cast<double>(sr) * speed)));
            next_deadline += chunk_dur;
            const auto now = std::chrono::steady_clock::now();
            if (next_deadline > now) {
                std::this_thread::sleep_until(next_deadline);
            } else {
                // Fell behind - re-anchor so we don't sleep negative forever.
                next_deadline = now;
            }
        }

        std::cout << "[file] Worker stopped" << std::endl;
    }

    std::unique_ptr<file_decoder::IDecoder> decoder_;
    SPSCRingBuffer<float>* capture_buffer_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<size_t> loop_count_{0};
    std::atomic<float> replay_speed_{1.0f};
    std::thread thread_;
};

// File-source factory
// Builds a decoder from a (path, format-hint) pair sent by the renderer.

struct LoadFileRequest {
    std::string path;
    std::string format;       // "wav" | "aiff" | "raw" | "f32" | "sigmf"
    // Only used for raw/f32/sigmf:
    std::string datatype;     // SigMF-style: "rf32_le", "ci16_le", etc.
    uint32_t    sample_rate = 0;
    uint32_t    channels    = 1;
};

inline std::unique_ptr<file_decoder::IDecoder> build_decoder(const LoadFileRequest& req) {
    using namespace file_decoder;
    if (req.format == "wav") {
        auto d = std::make_unique<WavDecoder>();
        if (!d->open(req.path)) {
            std::cerr << "[file] failed to open WAV: " << req.path << '\n';
            return nullptr;
        }
        return d;
    }
    if (req.format == "aiff") {
        auto d = std::make_unique<AiffDecoder>();
        if (!d->open(req.path)) {
            std::cerr << "[file] failed to open AIFF: " << req.path << '\n';
            return nullptr;
        }
        return d;
    }
    // raw / f32 / sigmf - caller must have supplied a datatype string.
    Datatype dt;
    if (!req.datatype.empty()) {
        if (!parse_sigmf_datatype(req.datatype, dt)) {
            std::cerr << "[file] invalid datatype: " << req.datatype << '\n';
            return nullptr;
        }
    } else if (req.format == "f32") {
        // .f32 is conventionally float32 little-endian, real, mono.
        dt.type = SampleType::F32;
        dt.little = true;
        dt.complex = false;
    } else {
        std::cerr << "[file] no datatype hint for " << req.format << '\n';
        return nullptr;
    }
    if (req.sample_rate > 0) dt.sample_rate = req.sample_rate;
    if (req.channels > 0)    dt.channels    = req.channels;

    auto d = std::make_unique<RawDecoder>();
    d->set_datatype(dt);
    if (!d->open(req.path)) {
        std::cerr << "[file] failed to open raw: " << req.path << '\n';
        return nullptr;
    }
    return d;
}

} // namespace signalscope
