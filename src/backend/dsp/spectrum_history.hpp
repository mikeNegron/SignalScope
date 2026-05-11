#pragma once
//
// SpectrumHistory - fixed-capacity ring of recent spectrum frames,
// served to the renderer on request_history when the user scrolls back
// past the GPU texture's working set. Full float precision (the
// renderer's u8 upload would lose the −90 to −130 dB band on rescale).
// Overflow drops oldest. An fft_size change clears the ring rather
// than silently mixing resolutions on the renderer's fixed-width tex.

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <deque>
#include <mutex>
#include <span>
#include <vector>

namespace signalscope {

class SpectrumHistory {
public:
    struct Row {
        std::vector<float> dB;          // length = fft_size (one- or two-sided)
        uint64_t           time_ns;     // monotonic clock at capture time
        uint16_t           fft_size;    // bin count (== dB.size())
        float              sample_rate;
        uint8_t            flags;       // FrameFlags bits at capture time
    };

    void set_capacity(std::size_t n_rows) {
        std::lock_guard<std::mutex> g(mu_);
        capacity_ = n_rows;
        while (ring_.size() > capacity_) ring_.pop_front();
    }
    std::size_t capacity() const {
        std::lock_guard<std::mutex> g(mu_);
        return capacity_;
    }
    std::size_t size() const {
        std::lock_guard<std::mutex> g(mu_);
        return ring_.size();
    }

    void clear() {
        std::lock_guard<std::mutex> g(mu_);
        ring_.clear();
    }

    void push(std::span<const float> dB, float sample_rate, uint8_t flags)
    {
        const auto t = std::chrono::steady_clock::now().time_since_epoch();
        const uint64_t time_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(t).count();

        std::lock_guard<std::mutex> g(mu_);

        // Resolution change: clear rather than mix resolutions in the ring.
        if (!ring_.empty() && ring_.back().fft_size != dB.size()) {
            ring_.clear();
        }

        Row r;
        r.dB.assign(dB.begin(), dB.end());
        r.time_ns     = time_ns;
        r.fft_size    = static_cast<uint16_t>(dB.size());
        r.sample_rate = sample_rate;
        r.flags       = flags;
        ring_.push_back(std::move(r));
        while (ring_.size() > capacity_) ring_.pop_front();
    }

    // Fetch up to `count` rows starting `start_offset` rows back from
    // newest (0 = newest). Returns newest-first; caller pre-sizes `out`.
    std::size_t fetch(std::size_t start_offset,
                      std::size_t count,
                      std::vector<Row>& out) const
    {
        out.clear();
        std::lock_guard<std::mutex> g(mu_);
        if (ring_.empty()) return 0;
        const std::size_t n = ring_.size();
        if (start_offset >= n) return 0;
        const std::size_t take = std::min(count, n - start_offset);
        out.reserve(take);
        for (std::size_t i = 0; i < take; i++) {
            // deque is oldest-at-front, so newest at idx n-1.
            const std::size_t idx = n - 1 - start_offset - i;
            out.push_back(ring_[idx]);
        }
        return take;
    }

private:
    mutable std::mutex mu_;
    std::deque<Row>    ring_;
    std::size_t        capacity_ = 4096;
};

} // namespace signalscope
