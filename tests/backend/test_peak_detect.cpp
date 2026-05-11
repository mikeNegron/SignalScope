// Unit tests for DSPPipeline::detect_peaks - the largest single
// algorithm we ship and (until now) entirely uncovered. The picker
// applies three filters in sequence:
//   1. Absolute gate:    value ≥ max(abs_min_db, global_median + floor_offset)
//   2. Shape:            strict local max at ±1, strict-or-equal at ±2
//   3. Local prominence: value − local-window median ≥ local_prom
// Then a greedy non-max suppression: highest-prominence candidate first,
// skip any subsequent candidate within `min_sep` bins of an already
// chosen peak. These tests pin each stage of that contract.

#include <gtest/gtest.h>

#include "dsp/dsp_pipeline.hpp"

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <utility>
#include <vector>

using signalscope::DSPPipeline;
using SpecPeak = DSPPipeline::SpecPeak;

namespace {

// Build a spectrum with a flat noise floor and explicit (bin, value)
// peaks. Single-bin peaks satisfy the picker's local-max test (strict
// rise vs neighbors at ±1, strict-or-equal at ±2) when the surrounding
// bins are all at the floor.
std::vector<float> make_spectrum(
    std::size_t N, float floorDb,
    std::initializer_list<std::pair<std::size_t, float>> peaks)
{
    std::vector<float> s(N, floorDb);
    for (const auto &kv : peaks) {
        if (kv.first < N) s[kv.first] = kv.second;
    }
    return s;
}

} // namespace

// Trivial cases
TEST(PeakDetect, EmptySpectrumReturnsNoPeaks) {
    DSPPipeline dsp;
    // N < 2*local_radius + 1 takes the early-out path.
    std::vector<float> s;
    auto &peaks = dsp.detect_peaks(s);
    EXPECT_EQ(peaks.size(), 0u);
}

TEST(PeakDetect, ShorterThanLocalWindowReturnsNoPeaks) {
    DSPPipeline dsp;
    // local_radius default 18 -> need N ≥ 37 just to enter the picker.
    std::vector<float> s(20, -20.0f);
    auto &peaks = dsp.detect_peaks(s);
    EXPECT_EQ(peaks.size(), 0u);
}

TEST(PeakDetect, FlatSpectrumReturnsNoPeaks) {
    DSPPipeline dsp;
    // Nothing rises above its neighbors; nothing has prominence.
    std::vector<float> s(1024, -50.0f);
    auto &peaks = dsp.detect_peaks(s);
    EXPECT_EQ(peaks.size(), 0u);
}

// Single-tone behavior
TEST(PeakDetect, SingleStrongToneDetectedAtCorrectBin) {
    DSPPipeline dsp;
    auto s = make_spectrum(1024, -120.0f, {{500, -20.0f}});
    auto &peaks = dsp.detect_peaks(s);
    ASSERT_EQ(peaks.size(), 1u);
    EXPECT_EQ(peaks[0].bin, 500u);
    EXPECT_FLOAT_EQ(peaks[0].value, -20.0f);
    EXPECT_GT(peaks[0].prominence, 50.0f); // huge rise above flat floor
}

TEST(PeakDetect, BelowAbsoluteGateNotDetected) {
    DSPPipeline dsp;
    // Default abs_min_db = −95. A peak at −100 fails the gate even
    // though it rises above the surrounding −120 floor.
    auto s = make_spectrum(1024, -120.0f, {{500, -100.0f}});
    auto &peaks = dsp.detect_peaks(s);
    EXPECT_EQ(peaks.size(), 0u);
}

TEST(PeakDetect, ProminenceBelowThresholdNotDetected) {
    DSPPipeline dsp;
    // A peak at −50 surrounded by a −55 plateau has only 5 dB local
    // prominence - below the default 10 dB threshold.
    std::vector<float> s(1024, -120.0f);
    for (int j = -18; j <= 18; j++) s[500 + j] = -55.0f;
    s[500] = -50.0f;
    auto &peaks = dsp.detect_peaks(s);
    EXPECT_EQ(peaks.size(), 0u);
}

// Multiple peaks
TEST(PeakDetect, ThreeWellSeparatedPeaksAllDetected) {
    DSPPipeline dsp;
    auto s = make_spectrum(1024, -120.0f,
        {{200, -10.0f}, {500, -30.0f}, {800, -20.0f}});
    auto &peaks = dsp.detect_peaks(s);
    EXPECT_EQ(peaks.size(), 3u);

    // Each input bin should appear somewhere in the output. Ordering
    // depends on prominence (greedy NMS), so check membership rather
    // than position.
    std::vector<std::size_t> bins;
    for (const auto &p : peaks) bins.push_back(p.bin);
    EXPECT_NE(std::find(bins.begin(), bins.end(), 200u), bins.end());
    EXPECT_NE(std::find(bins.begin(), bins.end(), 500u), bins.end());
    EXPECT_NE(std::find(bins.begin(), bins.end(), 800u), bins.end());
}

TEST(PeakDetect, OutputIsOrderedByProminenceDescending) {
    DSPPipeline dsp;
    auto s = make_spectrum(1024, -120.0f,
        {{200, -10.0f}, {500, -30.0f}, {800, -20.0f}});
    auto &peaks = dsp.detect_peaks(s);
    ASSERT_EQ(peaks.size(), 3u);
    EXPECT_GE(peaks[0].prominence, peaks[1].prominence);
    EXPECT_GE(peaks[1].prominence, peaks[2].prominence);
}

// Non-max suppression
TEST(PeakDetect, NMSDropsSecondPeakWithinMinSep) {
    DSPPipeline dsp;
    // For N=1024 the picker uses min_sep = max(6, ⌊N·0.006⌋) = 6.
    // Place peaks 3 bins apart (well inside the suppression radius):
    // both pass shape/prominence individually, but only the stronger
    // survives NMS.
    std::vector<float> s(1024, -120.0f);
    s[500] = -10.0f;
    s[503] = -25.0f;
    auto &peaks = dsp.detect_peaks(s);
    ASSERT_EQ(peaks.size(), 1u);
    EXPECT_EQ(peaks[0].bin, 500u);
}

TEST(PeakDetect, PeaksJustOutsideMinSepBothKept) {
    DSPPipeline dsp;
    // 7 bins apart -> ≥ min_sep, both should survive.
    std::vector<float> s(1024, -120.0f);
    s[500] = -10.0f;
    s[507] = -25.0f;
    auto &peaks = dsp.detect_peaks(s);
    EXPECT_EQ(peaks.size(), 2u);
}

// max_peaks cap
TEST(PeakDetect, MaxPeaksLimitsCount) {
    DSPPipeline dsp;
    // 12 well-separated tones; ask for only 4.
    std::vector<float> s(2048, -120.0f);
    for (std::size_t i = 0; i < 12; i++) {
        s[200 + i * 100] = -20.0f - static_cast<float>(i);
    }
    auto &peaks = dsp.detect_peaks(s, /*max_peaks=*/4);
    EXPECT_EQ(peaks.size(), 4u);

    // The 4 returned should be the 4 strongest by absolute value, since
    // the noise floor is uniform and prominence ≈ value − floor.
    for (const auto &p : peaks) EXPECT_GE(p.value, -23.0f);
}

// Boundary handling
TEST(PeakDetect, BinsInsideLocalRadiusOfEdgesAreSkipped) {
    DSPPipeline dsp;
    // Bins within local_radius=18 of either edge can't be picked
    // because the local-prominence window would walk off the array.
    std::vector<float> s(1024, -120.0f);
    s[5]    = -10.0f; // ≤ 18 from left edge
    s[1018] = -10.0f; // ≤ 18 from right edge
    auto &peaks = dsp.detect_peaks(s);
    EXPECT_EQ(peaks.size(), 0u);
}

// Pathological inputs
TEST(PeakDetect, NaNAndInfBinsAreIgnoredNotCrashed) {
    DSPPipeline dsp;
    std::vector<float> s(1024, -120.0f);
    s[100] = std::numeric_limits<float>::quiet_NaN();
    s[200] = std::numeric_limits<float>::infinity();
    s[300] = -std::numeric_limits<float>::infinity();
    s[500] = -20.0f; // single valid peak
    auto &peaks = dsp.detect_peaks(s);
    ASSERT_EQ(peaks.size(), 1u);
    EXPECT_EQ(peaks[0].bin, 500u);
}

// Tunable parameters
TEST(PeakDetect, RaisingAbsoluteGateExcludesWeakPeaks) {
    DSPPipeline dsp;
    auto s = make_spectrum(1024, -120.0f,
        {{300, -50.0f}, {600, -20.0f}});
    // Default gate (−95) catches both. Raise to −30 -> only the strong
    // peak qualifies.
    auto &peaks = dsp.detect_peaks(s,
        /*max_peaks=*/8, /*local_radius=*/18,
        /*abs_min_db=*/-30.0f);
    ASSERT_EQ(peaks.size(), 1u);
    EXPECT_EQ(peaks[0].bin, 600u);
}

TEST(PeakDetect, RaisingProminenceThresholdExcludesShallowPeaks) {
    DSPPipeline dsp;
    // Two peaks: one tall (huge rise), one shallow (5 dB rise).
    std::vector<float> s(1024, -120.0f);
    // Tall: bin 200, rises 100 dB above floor
    s[200] = -20.0f;
    // Shallow: bin 700, surroundings raised to -55, peak at -50 -> 5 dB rise
    for (int j = -18; j <= 18; j++) s[700 + j] = -55.0f;
    s[700] = -50.0f;

    // Default prom=10 -> only the tall one passes (the shallow one's 5 dB
    // is already too low). Verify.
    auto &peaks0 = dsp.detect_peaks(s);
    EXPECT_EQ(peaks0.size(), 1u);

    // Lower prom threshold to 3 dB -> both pass.
    auto &peaks1 = dsp.detect_peaks(s,
        8, 18, -95.0f, 18.0f, /*local_prom=*/3.0f);
    EXPECT_EQ(peaks1.size(), 2u);
}
