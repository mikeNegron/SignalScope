// Steady-state allocation count for serialize_frame_into. The whole
// reason the in-place API exists is so the broadcast hot path doesn't
// allocate after warm-up. This test pins that invariant so any future
// edit that re-introduces a per-frame alloc fails CI.

#include <gtest/gtest.h>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <vector>

#include "protocol/protocol.hpp"

namespace {
std::atomic<std::size_t> g_alloc_count{0};
bool g_counting = false;
}

// Replace global operator new / delete to count allocations. We only
// COUNT when g_counting is true so gtest's own setup doesn't pollute
// the number. This test must be the only TEST in its binary so the
// gtest infrastructure doesn't allocate during the counted window.
void* operator new(std::size_t n) {
    if (g_counting) g_alloc_count.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(n);
    if (!p) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

TEST(SerializeAllocCount, NoAllocsAfterWarmup) {
    using namespace signalscope;
    constexpr std::size_t N = 4096;       // half-N spectrum at FFT=8192
    constexpr std::size_t kCycles = 1000; // 1000 frames ≈ 16 seconds at 60 fps

    std::vector<float> payload(N, -90.0f);
    std::vector<uint8_t> dst;

    // Warm-up: one call to reach steady-state capacity.
    serialize_frame_into(dst, FrameType::Spectrum, payload.data(), N,
                         8192, 0, 48000.0f, 0);

    // Counted window.
    g_alloc_count.store(0, std::memory_order_relaxed);
    g_counting = true;
    for (std::size_t i = 0; i < kCycles; i++) {
        serialize_frame_into(dst, FrameType::Spectrum, payload.data(), N,
                             8192, static_cast<uint32_t>(i), 48000.0f, 0);
    }
    g_counting = false;

    EXPECT_EQ(0u, g_alloc_count.load(std::memory_order_relaxed))
        << "serialize_frame_into must not allocate after warm-up";
}
