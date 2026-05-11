// SpectrumHistory ring tests.
//
// Locking down:
//   - push() drops the oldest frame on capacity overflow
//   - fetch() returns newest-first from a given offset
//   - fft_size change clears the ring (we never mix resolutions)
//   - set_capacity shrinks the ring if needed
//   - clear() empties the ring

#include <gtest/gtest.h>

#include "dsp/spectrum_history.hpp"

#include <vector>

using signalscope::SpectrumHistory;

namespace {

std::vector<float> make_row(std::size_t n, float fill) {
    std::vector<float> v(n, fill);
    return v;
}

} // namespace

TEST(SpectrumHistory, EmptyByDefault) {
    SpectrumHistory h;
    EXPECT_EQ(h.size(), 0u);
}

TEST(SpectrumHistory, PushAddsRowsUpToCapacity) {
    SpectrumHistory h;
    h.set_capacity(4);
    for (int i = 0; i < 6; i++) {
        auto row = make_row(8, static_cast<float>(i));
        h.push(row, 48000.0f, 0);
    }
    // Should retain only the 4 newest: rows pushed with i=2..5.
    EXPECT_EQ(h.size(), 4u);

    std::vector<SpectrumHistory::Row> out;
    h.fetch(0, 4, out);
    ASSERT_EQ(out.size(), 4u);
    // fetch returns newest-first, so out[0] is the most recently pushed.
    EXPECT_FLOAT_EQ(out[0].dB[0], 5.0f);
    EXPECT_FLOAT_EQ(out[1].dB[0], 4.0f);
    EXPECT_FLOAT_EQ(out[2].dB[0], 3.0f);
    EXPECT_FLOAT_EQ(out[3].dB[0], 2.0f);
}

TEST(SpectrumHistory, FetchHonorsStartOffset) {
    SpectrumHistory h;
    h.set_capacity(10);
    for (int i = 0; i < 10; i++) {
        auto row = make_row(4, static_cast<float>(i));
        h.push(row, 48000.0f, 0);
    }
    std::vector<SpectrumHistory::Row> out;
    h.fetch(3, 2, out);
    ASSERT_EQ(out.size(), 2u);
    // Newest is 9; offset 3 = the row pushed with i=6; offset 4 = i=5.
    EXPECT_FLOAT_EQ(out[0].dB[0], 6.0f);
    EXPECT_FLOAT_EQ(out[1].dB[0], 5.0f);
}

TEST(SpectrumHistory, FetchPastEndReturnsWhatExists) {
    SpectrumHistory h;
    h.set_capacity(10);
    for (int i = 0; i < 3; i++) {
        auto row = make_row(4, static_cast<float>(i));
        h.push(row, 48000.0f, 0);
    }
    std::vector<SpectrumHistory::Row> out;
    h.fetch(1, 100, out);
    EXPECT_EQ(out.size(), 2u);  // only 2 rows behind the newest
}

TEST(SpectrumHistory, FftSizeChangeClearsRing) {
    SpectrumHistory h;
    h.set_capacity(10);
    for (int i = 0; i < 3; i++) {
        auto row = make_row(8, 1.0f);
        h.push(row, 48000.0f, 0);
    }
    EXPECT_EQ(h.size(), 3u);
    // Push at a different fft_size - should drop everything.
    auto row = make_row(16, 2.0f);
    h.push(row, 48000.0f, 0);
    EXPECT_EQ(h.size(), 1u);

    std::vector<SpectrumHistory::Row> out;
    h.fetch(0, 1, out);
    EXPECT_EQ(out[0].fft_size, 16u);
}

TEST(SpectrumHistory, SetCapacityShrinksRing) {
    SpectrumHistory h;
    h.set_capacity(10);
    for (int i = 0; i < 8; i++) {
        auto row = make_row(4, static_cast<float>(i));
        h.push(row, 48000.0f, 0);
    }
    EXPECT_EQ(h.size(), 8u);
    h.set_capacity(3);
    EXPECT_EQ(h.size(), 3u);

    std::vector<SpectrumHistory::Row> out;
    h.fetch(0, 3, out);
    ASSERT_EQ(out.size(), 3u);
    // Should have kept the 3 newest (rows 5, 6, 7).
    EXPECT_FLOAT_EQ(out[0].dB[0], 7.0f);
    EXPECT_FLOAT_EQ(out[1].dB[0], 6.0f);
    EXPECT_FLOAT_EQ(out[2].dB[0], 5.0f);
}

TEST(SpectrumHistory, ClearEmptiesRing) {
    SpectrumHistory h;
    h.set_capacity(4);
    auto row = make_row(4, 1.0f);
    h.push(row, 48000.0f, 0);
    EXPECT_EQ(h.size(), 1u);
    h.clear();
    EXPECT_EQ(h.size(), 0u);
}
