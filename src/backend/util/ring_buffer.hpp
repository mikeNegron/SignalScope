#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace signalscope {

/// Single-Producer Single-Consumer ring buffer.
///
/// Behavior:
/// - Producer writes with write().
/// - Consumer reads with read().
/// - If the producer writes more data than there is free space for,
///   the oldest unread samples are dropped.
/// - If the producer writes more than capacity(), only the last capacity()
///   samples from the input block are kept.
///
/// This is intended for real-time audio/DSP preview use, where keeping the
/// newest samples is more important than preserving every old sample.
template <typename T>
class SPSCRingBuffer {
    static_assert(std::is_trivially_copyable_v<T>,
                  "SPSCRingBuffer<T> requires T to be trivially copyable");

public:
    explicit SPSCRingBuffer(size_t capacity)
        : capacity_(checked_capacity(capacity))
        , mask_(capacity_ - 1)
        , buffer_(capacity_)
        , head_(0)
        , tail_(0)
    {}

    /// Available items to read.
    [[nodiscard]] size_t available() const noexcept {
        const size_t head = head_.load(std::memory_order_acquire);
        const size_t tail = tail_.load(std::memory_order_acquire);
        return head - tail;
    }

    /// Available space to write.
    [[nodiscard]] size_t free_space() const noexcept {
        return capacity_ - available();
    }

    [[nodiscard]] bool empty() const noexcept {
        return available() == 0;
    }

    [[nodiscard]] bool full() const noexcept {
        return free_space() == 0;
    }

    [[nodiscard]] size_t capacity() const noexcept {
        return capacity_;
    }

    /// Write a contiguous block.
    ///
    /// Returns the number of input items accepted. Marked nodiscard
    /// because the producer's "is everything in?" assumption is only
    /// safe when the returned count equals `count` - silently dropping
    /// the return masks the buffer-overrun path.
    ///
    /// If count > capacity(), only the newest capacity() items are kept.
    /// If the buffer does not have enough free space, oldest unread items
    /// are discarded to make room.
    [[nodiscard]] size_t write(const T* data, size_t count) noexcept {
        if (!data || count == 0) {
            return 0;
        }

        size_t n = count;

        // If the incoming block is larger than the whole buffer,
        // keep only the newest capacity_ samples.
        if (n > capacity_) {
            data += n - capacity_;
            n = capacity_;
        }

        make_room_for(n);

        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t pos = head & mask_;

        const size_t first = std::min(n, capacity_ - pos);

        for (size_t i = 0; i < first; ++i) {
            buffer_[pos + i].store(data[i], std::memory_order_relaxed);
        }

        for (size_t i = first; i < n; ++i) {
            buffer_[i - first].store(data[i], std::memory_order_relaxed);
        }

        head_.store(head + n, std::memory_order_release);
        return n;
    }

    /// Read a contiguous block.
    ///
    /// Returns the number of items actually read.
    [[nodiscard]] size_t read(T* dest, size_t count) noexcept {
        if (!dest || count == 0) {
            return 0;
        }

        while (true) {
            const size_t head = head_.load(std::memory_order_acquire);
            const size_t tail = tail_.load(std::memory_order_acquire);

            const size_t avail = head - tail;
            const size_t n = std::min(count, avail);

            if (n == 0) {
                return 0;
            }

            const size_t pos = tail & mask_;
            const size_t first = std::min(n, capacity_ - pos);

            for (size_t i = 0; i < first; ++i) {
                dest[i] = buffer_[pos + i].load(std::memory_order_relaxed);
            }

            for (size_t i = first; i < n; ++i) {
                dest[i] = buffer_[i - first].load(std::memory_order_relaxed);
            }

            // The producer may have dropped old samples while we were copying.
            // If tail_ is unchanged, the read is valid and we consume it.
            size_t expected_tail = tail;
            if (tail_.compare_exchange_weak(
                    expected_tail,
                    tail + n,
                    std::memory_order_release,
                    std::memory_order_acquire)) {
                return n;
            }

            // Otherwise, tail_ moved while we were reading. Retry from the new tail.
        }
    }

    /// Peek without consuming.
    ///
    /// Returns the number of items copied.
    [[nodiscard]] size_t peek(T* dest, size_t count) const noexcept {
        if (!dest || count == 0) {
            return 0;
        }

        while (true) {
            const size_t head = head_.load(std::memory_order_acquire);
            const size_t tail = tail_.load(std::memory_order_acquire);

            const size_t avail = head - tail;
            const size_t n = std::min(count, avail);

            if (n == 0) {
                return 0;
            }

            const size_t pos = tail & mask_;
            const size_t first = std::min(n, capacity_ - pos);

            for (size_t i = 0; i < first; ++i) {
                dest[i] = buffer_[pos + i].load(std::memory_order_relaxed);
            }

            for (size_t i = first; i < n; ++i) {
                dest[i] = buffer_[i - first].load(std::memory_order_relaxed);
            }

            // If tail did not move while peeking, the copied data is valid.
            if (tail_.load(std::memory_order_acquire) == tail) {
                return n;
            }

            // Producer dropped old data while we peeked. Retry.
        }
    }

    /// Skip up to count items without reading them.
    [[nodiscard]] size_t skip(size_t count) noexcept {
        if (count == 0) {
            return 0;
        }

        while (true) {
            const size_t head = head_.load(std::memory_order_acquire);
            const size_t tail = tail_.load(std::memory_order_acquire);

            const size_t avail = head - tail;
            const size_t n = std::min(count, avail);

            if (n == 0) {
                return 0;
            }

            size_t expected_tail = tail;
            if (tail_.compare_exchange_weak(
                    expected_tail,
                    tail + n,
                    std::memory_order_release,
                    std::memory_order_acquire)) {
                return n;
            }
        }
    }

    /// Reset to empty state.
    ///
    /// Only safe when both producer and consumer are idle.
    void reset() noexcept {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

private:
    static size_t checked_capacity(size_t requested) {
        if (requested == 0) {
            throw std::invalid_argument("SPSCRingBuffer capacity must be greater than zero");
        }

        constexpr size_t max_power_of_two =
            size_t{1} << (std::numeric_limits<size_t>::digits - 1);

        if (requested > max_power_of_two) {
            throw std::length_error("SPSCRingBuffer capacity is too large");
        }

        return std::bit_ceil(requested);
    }

    /// Make room for n items by dropping oldest unread data if needed.
    void make_room_for(size_t n) noexcept {
        assert(n <= capacity_);

        while (true) {
            const size_t head = head_.load(std::memory_order_relaxed);
            const size_t tail = tail_.load(std::memory_order_acquire);

            const size_t used = head - tail;
            const size_t free = capacity_ - used;

            if (free >= n) {
                return;
            }

            const size_t to_drop = n - free;
            const size_t new_tail = tail + to_drop;

            size_t expected_tail = tail;
            if (tail_.compare_exchange_weak(
                    expected_tail,
                    new_tail,
                    std::memory_order_release,
                    std::memory_order_acquire)) {
                return;
            }
        }
    }

    const size_t capacity_;
    const size_t mask_;

    std::vector<std::atomic<T>> buffer_;

    alignas(64) std::atomic<size_t> head_;
    alignas(64) std::atomic<size_t> tail_;
};

} // namespace signalscope