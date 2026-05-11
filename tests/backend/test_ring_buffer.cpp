// SPSCRingBuffer tests - both single-threaded contract and a property
// stress test under real producer/consumer threads.
// What we're locking down:
//   - Capacity rounds up to power-of-two; throws on 0
//   - Round-trip: write + read returns the same data in order
//   - Available / free_space / empty / full match write/read state
//   - Wrap-around: writes that cross the buffer boundary stay correct
//   - Overflow drops oldest, keeps newest (it's a real-time scope buffer,
//     not a queue)
//   - peek() doesn't consume
//   - skip() advances the read pointer without copying
//   - reset() returns to empty
//   - Concurrent producer/consumer: every byte that the producer wrote
//     and the consumer received was an actual produced byte (never
//     fabricated), and the order it landed in matches what was sent.

#include <gtest/gtest.h>

#include <tuple>

#include "util/ring_buffer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <thread>
#include <vector>

using signalscope::SPSCRingBuffer;
using namespace std::chrono_literals;

// Construction
TEST(SPSCRingBuffer, ZeroCapacityThrows) {
    EXPECT_THROW(SPSCRingBuffer<int>{0}, std::invalid_argument);
}

TEST(SPSCRingBuffer, CapacityRoundsUpToPowerOfTwo) {
    SPSCRingBuffer<int> a(1);   EXPECT_EQ(a.capacity(), 1u);
    SPSCRingBuffer<int> b(2);   EXPECT_EQ(b.capacity(), 2u);
    SPSCRingBuffer<int> c(3);   EXPECT_EQ(c.capacity(), 4u);
    SPSCRingBuffer<int> d(5);   EXPECT_EQ(d.capacity(), 8u);
    SPSCRingBuffer<int> e(100); EXPECT_EQ(e.capacity(), 128u);
}

TEST(SPSCRingBuffer, FreshBufferIsEmpty) {
    SPSCRingBuffer<int> rb(16);
    EXPECT_TRUE(rb.empty());
    EXPECT_FALSE(rb.full());
    EXPECT_EQ(rb.available(), 0u);
    EXPECT_EQ(rb.free_space(), 16u);
}

// Round-trip
TEST(SPSCRingBuffer, WriteReadRoundTripPreservesOrder) {
    SPSCRingBuffer<int> rb(16);
    const std::vector<int> in = {1, 2, 3, 4, 5};
    EXPECT_EQ(rb.write(in.data(), in.size()), in.size());
    EXPECT_EQ(rb.available(), in.size());

    std::vector<int> out(in.size());
    EXPECT_EQ(rb.read(out.data(), out.size()), in.size());
    EXPECT_EQ(out, in);
    EXPECT_TRUE(rb.empty());
}

TEST(SPSCRingBuffer, ReadOnEmptyReturnsZero) {
    SPSCRingBuffer<int> rb(16);
    int x = 0;
    EXPECT_EQ(rb.read(&x, 1), 0u);
}

TEST(SPSCRingBuffer, NullPointersAreNoOps) {
    SPSCRingBuffer<int> rb(16);
    EXPECT_EQ(rb.write(nullptr, 5), 0u);
    EXPECT_EQ(rb.read(nullptr, 5), 0u);
}

// Wrap-around
TEST(SPSCRingBuffer, WriteWrapsAroundCleanly) {
    // Capacity = 8. Write 6, read 6, write 6 more - second write spans
    // the wrap boundary at index 6->7->0->1.
    SPSCRingBuffer<int> rb(8);
    std::vector<int> a = {1, 2, 3, 4, 5, 6};
    std::ignore = rb.write(a.data(), a.size());
    std::vector<int> tmp(6);
    std::ignore = rb.read(tmp.data(), tmp.size());

    std::vector<int> b = {10, 20, 30, 40, 50, 60};
    EXPECT_EQ(rb.write(b.data(), b.size()), b.size());
    std::vector<int> out(b.size());
    EXPECT_EQ(rb.read(out.data(), out.size()), b.size());
    EXPECT_EQ(out, b);
}

// Overflow semantics: drop oldest, keep newest
TEST(SPSCRingBuffer, OverflowDropsOldestKeepsNewest) {
    // Test the make_room_for path - earlier writes get dropped when a
    // later write would otherwise overflow.
    SPSCRingBuffer<int> rb(4);
    const std::vector<int> a = {1, 2, 3};
    EXPECT_EQ(rb.write(a.data(), a.size()), a.size());
    const std::vector<int> b = {4, 5, 6};
    EXPECT_EQ(rb.write(b.data(), b.size()), b.size());
    // Buffer capacity is 4. We've now pushed 6 items total - items 1
    // and 2 should have been dropped to make room, leaving [3,4,5,6].
    EXPECT_EQ(rb.available(), 4u);
    std::vector<int> out(4);
    EXPECT_EQ(rb.read(out.data(), 4), 4u);
    EXPECT_EQ(out, (std::vector<int>{3, 4, 5, 6}));
}

TEST(SPSCRingBuffer, OversizedSingleWriteKeepsLastCapacityItems) {
    SPSCRingBuffer<int> rb(4);
    const std::vector<int> in = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    EXPECT_EQ(rb.write(in.data(), in.size()), 4u);  // returned == kept

    std::vector<int> out(4);
    EXPECT_EQ(rb.read(out.data(), 4), 4u);
    EXPECT_EQ(out, (std::vector<int>{7, 8, 9, 10}));
}

// peek / skip
TEST(SPSCRingBuffer, PeekDoesNotConsume) {
    SPSCRingBuffer<int> rb(16);
    std::vector<int> in = {7, 8, 9};
    std::ignore = rb.write(in.data(), in.size());

    std::vector<int> peeked(3);
    EXPECT_EQ(rb.peek(peeked.data(), 3), 3u);
    EXPECT_EQ(peeked, in);
    EXPECT_EQ(rb.available(), 3u);  // unchanged

    // Read still gets the same data.
    std::vector<int> out(3);
    EXPECT_EQ(rb.read(out.data(), 3), 3u);
    EXPECT_EQ(out, in);
}

TEST(SPSCRingBuffer, SkipAdvancesReadPointer) {
    SPSCRingBuffer<int> rb(16);
    std::vector<int> in = {1, 2, 3, 4, 5};
    std::ignore = rb.write(in.data(), in.size());

    EXPECT_EQ(rb.skip(2), 2u);
    EXPECT_EQ(rb.available(), 3u);

    std::vector<int> out(3);
    EXPECT_EQ(rb.read(out.data(), 3), 3u);
    EXPECT_EQ(out, (std::vector<int>{3, 4, 5}));
}

// reset
TEST(SPSCRingBuffer, ResetClearsState) {
    SPSCRingBuffer<int> rb(16);
    std::vector<int> in = {1, 2, 3};
    std::ignore = rb.write(in.data(), in.size());
    rb.reset();
    EXPECT_TRUE(rb.empty());
    EXPECT_EQ(rb.available(), 0u);
}

// Concurrency: producer + consumer property test
// Pumps a known stream of integers through the buffer with the producer
// and consumer running on real threads. The stream is i for i in
// [0, N). At the end we check every value the consumer collected was
// non-decreasing and that its final value reached N-1 (i.e., the
// consumer kept up enough to see the last item). We can't assert "no
// drops" because the buffer's overflow-drops-oldest policy is by
// design - instead we check that whatever the consumer DID receive
// matches a strictly ordered subset of the producer's output.
TEST(SPSCRingBuffer, ConcurrentProducerConsumerOrderingHolds) {
    constexpr int CAPACITY = 1024;
    constexpr int N = 200000;

    SPSCRingBuffer<int> rb(CAPACITY);
    std::atomic<bool> producer_done{false};
    std::vector<int> received;
    received.reserve(N);

    std::thread producer([&] {
        for (int i = 0; i < N; i++) {
            std::ignore = rb.write(&i, 1);
            // Tiny breather every so often so the consumer gets a chance.
            if ((i & 0xFFF) == 0) std::this_thread::yield();
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        std::vector<int> chunk(64);
        while (!producer_done.load(std::memory_order_acquire) || !rb.empty()) {
            const std::size_t n = rb.read(chunk.data(), chunk.size());
            for (std::size_t i = 0; i < n; i++) received.push_back(chunk[i]);
            if (n == 0) std::this_thread::yield();
        }
    });

    producer.join();
    consumer.join();

    // Property 1: every received value is in [0, N).
    for (int v : received) {
        EXPECT_GE(v, 0);
        EXPECT_LT(v, N);
    }
    // Property 2: strictly increasing - buffer never reorders or
    // duplicates. Drops are allowed; out-of-order is not.
    for (std::size_t i = 1; i < received.size(); i++) {
        ASSERT_GT(received[i], received[i - 1])
            << "Out-of-order at index " << i << ": "
            << received[i - 1] << " then " << received[i];
    }
    // Property 3: consumer received the very last produced value (or
    // close to it). Even if drops happened mid-run, the producer's
    // final write should still be there since the consumer keeps
    // draining until producer_done AND empty.
    if (!received.empty()) {
        EXPECT_EQ(received.back(), N - 1);
    }
}

// Higher-throughput version with no per-item yield to stress the
// drop-oldest path. Just asserts ordering - drops are expected.
TEST(SPSCRingBuffer, ConcurrentBurstWriteOrderingHolds) {
    constexpr int CAPACITY = 64;       // tiny - guaranteed drops
    constexpr int N = 100000;

    SPSCRingBuffer<int> rb(CAPACITY);
    std::atomic<bool> producer_done{false};
    std::vector<int> received;
    received.reserve(N);

    std::thread producer([&] {
        for (int i = 0; i < N; i++) std::ignore = rb.write(&i, 1);
        producer_done.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        int buf;
        while (!producer_done.load(std::memory_order_acquire) || !rb.empty()) {
            if (rb.read(&buf, 1) == 1) received.push_back(buf);
        }
    });

    producer.join();
    consumer.join();

    for (std::size_t i = 1; i < received.size(); i++) {
        ASSERT_GT(received[i], received[i - 1])
            << "Out-of-order at " << i;
    }
}
