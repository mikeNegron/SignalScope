// DownConverter (NCO + windowed-sinc LPF + decimator) tests.
// Three independent stages, plus their interaction with the FIR's
// startup transient. The properties we want to lock down:
//   1. Inactive (tune=0, decim=1) is a true pass-through for the real
//      part: output_re ≈ input, output_im ≈ 0.
//   2. NCO mixing shifts a known sinusoid to the right baseband:
//      a tone at f₀ tuned to f₀ ends up at DC.
//   3. Decimation reduces output length by exactly M.
//   4. Reset clears the FIR delay line + NCO phase.
//   5. process_complex tune=0 round-trips identity.

#include <gtest/gtest.h>

#include "dsp/downconverter.hpp"

#include <cmath>
#include <cstddef>
#include <vector>

using signalscope::DownConverter;
using namespace std::numbers;

namespace {

// FIR group delay - same constant the implementation uses (FIR_LEN/2 of 65).
constexpr std::size_t FIR_GD = 32;

// Build a unit-amplitude cosine of length N at frequency f0 (in Hz, given fs).
std::vector<float> cosine(std::size_t N, double f0, double fs) {
    std::vector<float> x(N);
    const double w = 2.0 * pi * f0 / fs;
    for (std::size_t i = 0; i < N; i++) x[i] = std::cos(w * i);
    return x;
}

// Same length, but the matching sine - together (cos, sin) form an
// analytic signal exp(j·2π·f0·t) which has constant magnitude 1.
// Used to test frequency translation without mirror-image confusion.
std::vector<float> sine(std::size_t N, double f0, double fs) {
    std::vector<float> x(N);
    const double w = 2.0 * pi * f0 / fs;
    for (std::size_t i = 0; i < N; i++) x[i] = std::sin(w * i);
    return x;
}

// Average magnitude over a steady-state window - useful for tone-detection
// after the FIR's startup transient has cleared.
double rms_magnitude(const std::vector<float>& re,
                     const std::vector<float>& im,
                     std::size_t skip)
{
    double sum = 0.0;
    std::size_t cnt = 0;
    for (std::size_t i = skip; i < re.size(); i++) {
        sum += std::sqrt(double(re[i]) * re[i] + double(im[i]) * im[i]);
        cnt++;
    }
    return cnt > 0 ? sum / cnt : 0.0;
}

} // namespace

// active() / effective_sample_rate
TEST(DownConverter, ActiveFalseWhenIdentityConfig) {
    DownConverter ddc;
    ddc.configure(/*tune=*/0.0f, /*decim=*/1, /*sr=*/48000.0f);
    EXPECT_FALSE(ddc.active());
}

TEST(DownConverter, ActiveTrueWhenTuneNonZero) {
    DownConverter ddc;
    ddc.configure(1000.0f, 1, 48000.0f);
    EXPECT_TRUE(ddc.active());
}

TEST(DownConverter, ActiveTrueWhenDecimationGreaterThanOne) {
    DownConverter ddc;
    ddc.configure(0.0f, 4, 48000.0f);
    EXPECT_TRUE(ddc.active());
}

TEST(DownConverter, EffectiveSampleRateMatchesDecim) {
    DownConverter ddc;
    ddc.configure(0.0f, 4, 48000.0f);
    EXPECT_FLOAT_EQ(ddc.effective_sample_rate(), 12000.0f);
    ddc.configure(0.0f, 1, 48000.0f);
    EXPECT_FLOAT_EQ(ddc.effective_sample_rate(), 48000.0f);
}

TEST(DownConverter, DecimationFloorsToOne) {
    DownConverter ddc;
    ddc.configure(0.0f, /*decim=*/0, 48000.0f);
    EXPECT_EQ(ddc.decimation(), 1u);
}

// Pass-through (tune=0, decim=1)
TEST(DownConverter, RealPassthroughLeavesImagZero) {
    DownConverter ddc;
    ddc.configure(0.0f, 1, 48000.0f);
    auto x = cosine(2048, 1000.0, 48000.0);
    std::vector<float> re, im;
    ddc.process_real(x, re, im);

    ASSERT_EQ(re.size(), x.size());
    ASSERT_EQ(im.size(), x.size());
    // After the FIR delay, the imaginary output is identically zero
    // because the NCO multiplier is exp(0) = 1+j·0.
    for (std::size_t i = FIR_GD * 2; i < im.size(); i++) {
        EXPECT_EQ(im[i], 0.0f);
    }
}

TEST(DownConverter, ComplexPassthroughRoundTrips) {
    DownConverter ddc;
    ddc.configure(0.0f, 1, 48000.0f);
    // Analytic input exp(j·2π·1000·t) - has constant magnitude 1.
    auto re_in = cosine(2048, 1000.0, 48000.0);
    auto im_in = sine  (2048, 1000.0, 48000.0);
    std::vector<float> re_out, im_out;
    ddc.process_complex(re_in, im_in,
                        re_out, im_out);
    ASSERT_EQ(re_out.size(), re_in.size());

    // Past the FIR ramp, the output is a delayed copy of the analytic
    // input - magnitude should remain ~1 since the windowed-sinc has
    // near-unity gain in-band.
    const double mag = rms_magnitude(re_out, im_out, /*skip=*/4 * FIR_GD);
    EXPECT_NEAR(mag, 1.0, 0.05);
}

// Tune (frequency translation)
TEST(DownConverter, TuneToToneFrequencyShiftsAnalyticSignalToDC) {
    DownConverter ddc;
    const double fs = 48000.0;
    const double f0 = 1000.0;
    ddc.configure(static_cast<float>(f0), 1, static_cast<float>(fs));

    // Analytic input at +f0: exp(j·ω₀·t). Mixing by exp(-j·ω₀·t) gives
    // exp(0) = 1 (DC), constant magnitude 1. No mirror to worry about.
    auto re_in = cosine(8192, f0, fs);
    auto im_in = sine  (8192, f0, fs);
    std::vector<float> re, im;
    ddc.process_complex(re_in, im_in, re, im);

    const double mag = rms_magnitude(re, im, /*skip=*/4 * FIR_GD);
    EXPECT_NEAR(mag, 1.0, 0.05);
}

TEST(DownConverter, TuneAwayFromToneAttenuatesAfterDecimation) {
    DownConverter ddc;
    const double fs = 48000.0;
    // Decim=4 gives effective Nyquist = 6 kHz. Mix-output at +20 kHz is
    // way outside that passband, so the FIR + decimator should bury it.
    ddc.configure(/*tune=*/-10000.0f, /*decim=*/4, static_cast<float>(fs));

    auto re_in = cosine(8192, 10000.0, fs); // +10 kHz analytic input
    auto im_in = sine  (8192, 10000.0, fs);
    std::vector<float> re, im;
    ddc.process_complex(re_in, im_in, re, im);

    const double mag = rms_magnitude(re, im, /*skip=*/8 * FIR_GD);
    EXPECT_LT(mag, 0.1); // well below the in-band magnitude of ~1
}

// Decimation
TEST(DownConverter, OutputLengthDividesByDecim) {
    DownConverter ddc;
    ddc.configure(0.0f, 4, 48000.0f);
    std::vector<float> in(4096, 0.0f);
    std::vector<float> re, im;
    ddc.process_real(in, re, im);
    EXPECT_EQ(re.size(), in.size() / 4);
    EXPECT_EQ(im.size(), in.size() / 4);
}

TEST(DownConverter, DecimByEightProducesRightCount) {
    DownConverter ddc;
    ddc.configure(0.0f, 8, 48000.0f);
    std::vector<float> in(8192, 0.0f);
    std::vector<float> re, im;
    ddc.process_real(in, re, im);
    EXPECT_EQ(re.size(), 1024u);
}

// reset()
TEST(DownConverter, ResetClearsFirHistoryAndPhase) {
    DownConverter ddc;
    const double fs = 48000.0;
    ddc.configure(1000.0f, 1, static_cast<float>(fs));
    auto x = cosine(2048, 1000.0, fs);
    std::vector<float> re1, im1;
    ddc.process_real(x, re1, im1);

    // After processing 2048 samples, internal state is non-zero. Reset
    // and process again - output should match the original first run
    // bit-for-bit because reset() restores both the FIR delay line and
    // the NCO phase to their initial values.
    ddc.reset();
    std::vector<float> re2, im2;
    ddc.process_real(x, re2, im2);

    ASSERT_EQ(re1.size(), re2.size());
    for (std::size_t i = 0; i < re1.size(); i++) {
        EXPECT_FLOAT_EQ(re1[i], re2[i]) << "sample " << i;
        EXPECT_FLOAT_EQ(im1[i], im2[i]) << "sample " << i;
    }
}

// State carries across calls
TEST(DownConverter, StreamingMatchesOneShotForTuneProcessing) {
    // The FIR delay line + NCO phase persist across process_real() calls
    // so back-to-back blocks splice cleanly. Verify by comparing one
    // 2048-sample call against two 1024-sample calls - the second half
    // of the streaming output must equal the second half of the one-shot.
    const double fs = 48000.0;
    auto x = cosine(2048, 1000.0, fs);

    DownConverter a, b;
    a.configure(1000.0f, 1, static_cast<float>(fs));
    b.configure(1000.0f, 1, static_cast<float>(fs));

    std::vector<float> re_oneshot, im_oneshot;
    a.process_real(x, re_oneshot, im_oneshot);

    std::vector<float> re_a, im_a, re_b, im_b;
    b.process_real({x.data(), 1024}, re_a, im_a);
    b.process_real({x.data() + 1024, 1024}, re_b, im_b);

    // Concatenate streamed halves
    std::vector<float> re_stream, im_stream;
    re_stream.insert(re_stream.end(), re_a.begin(), re_a.end());
    re_stream.insert(re_stream.end(), re_b.begin(), re_b.end());
    im_stream.insert(im_stream.end(), im_a.begin(), im_a.end());
    im_stream.insert(im_stream.end(), im_b.begin(), im_b.end());

    ASSERT_EQ(re_oneshot.size(), re_stream.size());
    for (std::size_t i = 0; i < re_oneshot.size(); i++) {
        EXPECT_FLOAT_EQ(re_oneshot[i], re_stream[i]) << "sample " << i;
        EXPECT_FLOAT_EQ(im_oneshot[i], im_stream[i]) << "sample " << i;
    }
}
