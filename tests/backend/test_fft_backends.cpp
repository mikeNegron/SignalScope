// Correctness suite parameterized over every FFT backend compiled into
// this test binary. Catches backend-specific bugs (normalization, output
// layout, complex packing) by re-running the same five reference checks
// against pocketfft, FFTW, and KFR.
//
// No sockets, no subprocesses, no global state across tests beyond
// ss::fft::initialize() — which we re-call in SetUp for each param so
// the backend choice is deterministic. Cleanup is automatic via RAII
// on the IFftBackend unique_ptr.

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <complex>
#include <iostream>
#include <numbers>
#include <string>
#include <vector>

#include "dsp/fft/fft_backend.hpp"

namespace {

constexpr float kPi = std::numbers::pi_v<float>;

// Returns the list of backend names compiled into this binary. pocketfft
// is always present; the others appear only when their CMake flag was set.
std::vector<std::string> compiled_backends() {
    std::vector<std::string> names = {"pocketfft"};
#ifdef SIGNALSCOPE_HAVE_FFTW
    names.emplace_back("fftw");
#endif
#ifdef SIGNALSCOPE_HAVE_KFR
    names.emplace_back("kfr");
#endif
    return names;
}

class FftBackendTest : public ::testing::TestWithParam<std::string> {
protected:
    void SetUp() override {
        ss::fft::initialize(GetParam());
        ss::fft::global_startup();
        backend_ = ss::fft::make_backend();
        ASSERT_NE(backend_, nullptr);
    }

    void TearDown() override {
        // Drop the backend first (releases buffers/plans) before any
        // backend-global state is torn down. unique_ptr handles the rest.
        backend_.reset();
    }

    std::unique_ptr<ss::fft::IFftBackend> backend_;
};

TEST_P(FftBackendTest, ReportsExpectedName) {
    EXPECT_EQ(backend_->name(), GetParam());
}

// Reproduces the production worst-case size. Used to be missing — bug report
// surfaced that file replay + N=32768 froze the displays with the KFR
// backend. Asserts resize + execute complete in bounded wall-clock time.
TEST_P(FftBackendTest, LargeSize32kCompletesInBoundedTime) {
    constexpr std::size_t N = 32768;
    using clk = std::chrono::steady_clock;

    auto t0 = clk::now();
    backend_->resize(N);
    auto t1 = clk::now();

    float *in = backend_->input_real();
    for (std::size_t i = 0; i < N; i++) {
        in[i] = std::cos(2.0f * kPi * 137.0f * static_cast<float>(i) /
                         static_cast<float>(N));
    }

    backend_->execute_r2c();
    auto t2 = clk::now();
    backend_->execute_r2c();
    auto t3 = clk::now();

    const double resize_ms  = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double first_ms   = std::chrono::duration<double, std::milli>(t2 - t1).count();
    const double second_ms  = std::chrono::duration<double, std::milli>(t3 - t2).count();

    // 5 s ceiling is intentionally generous; real budgets are sub-100 ms
    // even on slow boxes. Exceeding this almost certainly means the backend
    // hangs at the production FFT size.
    EXPECT_LT(resize_ms, 5000.0)  << "resize(32768) took " << resize_ms << " ms";
    EXPECT_LT(first_ms,  5000.0)  << "first execute_r2c took " << first_ms << " ms";
    EXPECT_LT(second_ms, 5000.0)  << "second execute_r2c took " << second_ms << " ms";

    std::cerr << "  [N=32768/" << GetParam() << "] resize="
              << resize_ms << "ms exec1=" << first_ms
              << "ms exec2=" << second_ms << "ms\n";
}

// Impulse: delta at sample 0, all real input -> magnitude is flat at 1.0
// across every bin of the one-sided spectrum.
TEST_P(FftBackendTest, R2cImpulseResponseIsFlat) {
    constexpr std::size_t N = 1024;
    backend_->resize(N);

    float* in = backend_->input_real();
    in[0] = 1.0f;
    for (std::size_t i = 1; i < N; i++) in[i] = 0.0f;

    backend_->execute_r2c();

    const std::complex<float>* out = backend_->output_r2c();
    for (std::size_t i = 0; i < N / 2 + 1; i++) {
        const float mag = std::abs(out[i]);
        EXPECT_NEAR(mag, 1.0f, 1e-4f) << "bin " << i;
    }
}

// DC: constant 1.0 input -> bin 0 has magnitude N, all other bins zero.
TEST_P(FftBackendTest, R2cDcBinCapturesAllEnergy) {
    constexpr std::size_t N = 2048;
    backend_->resize(N);

    float* in = backend_->input_real();
    for (std::size_t i = 0; i < N; i++) in[i] = 1.0f;

    backend_->execute_r2c();

    const std::complex<float>* out = backend_->output_r2c();
    EXPECT_NEAR(std::abs(out[0]), static_cast<float>(N), 1e-2f);
    for (std::size_t i = 1; i < N / 2 + 1; i++) {
        EXPECT_NEAR(std::abs(out[i]), 0.0f, 1e-2f) << "bin " << i;
    }
}

// Single tone: sine wave at bin k -> peak at bin k. Amplitude check is
// loose to absorb leakage; we only assert the peak bin is k.
TEST_P(FftBackendTest, R2cSingleTonePeaksAtCorrectBin) {
    constexpr std::size_t N = 4096;
    constexpr std::size_t target_bin = 137;
    backend_->resize(N);

    float* in = backend_->input_real();
    for (std::size_t i = 0; i < N; i++) {
        in[i] = std::cos(2.0f * kPi * static_cast<float>(target_bin) *
                         static_cast<float>(i) / static_cast<float>(N));
    }

    backend_->execute_r2c();

    const std::complex<float>* out = backend_->output_r2c();
    std::size_t peak_bin = 0;
    float peak_mag = 0.0f;
    for (std::size_t i = 0; i < N / 2 + 1; i++) {
        const float mag = std::abs(out[i]);
        if (mag > peak_mag) { peak_mag = mag; peak_bin = i; }
    }
    EXPECT_EQ(peak_bin, target_bin);
    // A unit-amplitude real cosine at an integer bin places N/2 in that
    // bin (the conjugate-symmetric pair sums there). Allow 1% slack.
    EXPECT_NEAR(peak_mag, static_cast<float>(N) / 2.0f,
                static_cast<float>(N) * 0.01f);
}

// Parseval: sum(|x|^2) == (1/N) * sum(|X|^2) for an unnormalized DFT,
// where X[N-k] = conj(X[k]) for real input — so the one-sided sum must
// double-count interior bins.
TEST_P(FftBackendTest, R2cParsevalIdentityHolds) {
    constexpr std::size_t N = 2048;
    backend_->resize(N);

    float* in = backend_->input_real();
    double time_energy = 0.0;
    for (std::size_t i = 0; i < N; i++) {
        const float v = std::sin(2.0f * kPi * 13.0f *
                                 static_cast<float>(i) / static_cast<float>(N))
                      + 0.5f * std::sin(2.0f * kPi * 41.0f *
                                        static_cast<float>(i) / static_cast<float>(N));
        in[i] = v;
        time_energy += static_cast<double>(v) * static_cast<double>(v);
    }

    backend_->execute_r2c();

    const std::complex<float>* out = backend_->output_r2c();
    double freq_energy = 0.0;
    for (std::size_t i = 0; i < N / 2 + 1; i++) {
        const double mag2 = static_cast<double>(std::norm(out[i]));
        // DC and Nyquist appear once; interior bins represent two
        // conjugate-symmetric pairs and must be doubled.
        const bool interior = (i != 0 && i != N / 2);
        freq_energy += interior ? 2.0 * mag2 : mag2;
    }
    freq_energy /= static_cast<double>(N);

    EXPECT_NEAR(freq_energy, time_energy,
                std::max(1e-1, 1e-4 * time_energy));
}

// c2c forward: a pure complex exponential e^{j*2*pi*k*n/N} -> a single
// non-zero bin at index k with magnitude N.
TEST_P(FftBackendTest, C2cComplexExponentialPicksOneBin) {
    constexpr std::size_t N = 1024;
    constexpr std::size_t target_bin = 87;
    backend_->resize(N);

    std::complex<float>* in = backend_->input_complex();
    for (std::size_t i = 0; i < N; i++) {
        const float phase = 2.0f * kPi * static_cast<float>(target_bin) *
                            static_cast<float>(i) / static_cast<float>(N);
        in[i] = std::complex<float>(std::cos(phase), std::sin(phase));
    }

    backend_->execute_c2c_forward();

    const std::complex<float>* out = backend_->output_complex();
    EXPECT_NEAR(std::abs(out[target_bin]), static_cast<float>(N),
                static_cast<float>(N) * 0.01f);
    for (std::size_t i = 0; i < N; i++) {
        if (i == target_bin) continue;
        EXPECT_NEAR(std::abs(out[i]), 0.0f,
                    static_cast<float>(N) * 0.01f) << "leakage at bin " << i;
    }
}

INSTANTIATE_TEST_SUITE_P(
    AllCompiledBackends,
    FftBackendTest,
    ::testing::ValuesIn(compiled_backends()),
    [](const ::testing::TestParamInfo<std::string>& info) {
        return info.param;
    });

// Cross-backend consistency: when more than one backend is compiled in,
// any two backends must agree (within float tolerance) on the same input.
// Skipped automatically when only pocketfft is present.
TEST(FftBackendCrossCheck, BackendsAgreeWithinTolerance) {
    const auto names = compiled_backends();
    if (names.size() < 2) {
        GTEST_SKIP() << "only one backend compiled in";
    }

    constexpr std::size_t N = 2048;
    std::vector<float> input(N);
    for (std::size_t i = 0; i < N; i++) {
        input[i] = std::cos(2.0f * kPi * 17.0f *
                            static_cast<float>(i) / static_cast<float>(N))
                 + 0.3f * std::cos(2.0f * kPi * 53.0f *
                                   static_cast<float>(i) / static_cast<float>(N))
                 + 0.1f * static_cast<float>(i % 7) / 7.0f;
    }

    auto run = [&](const std::string& name) {
        ss::fft::initialize(name);
        ss::fft::global_startup();
        auto be = ss::fft::make_backend();
        be->resize(N);
        std::copy(input.begin(), input.end(), be->input_real());
        be->execute_r2c();
        std::vector<std::complex<float>> out(N / 2 + 1);
        std::copy(be->output_r2c(), be->output_r2c() + (N / 2 + 1), out.begin());
        return out;
    };

    const auto ref = run(names[0]);
    for (std::size_t k = 1; k < names.size(); k++) {
        const auto cmp = run(names[k]);
        ASSERT_EQ(ref.size(), cmp.size());
        for (std::size_t i = 0; i < ref.size(); i++) {
            const float diff = std::abs(ref[i] - cmp[i]);
            const float scale = std::max(1.0f, std::abs(ref[i]));
            EXPECT_LT(diff / scale, 1e-3f)
                << names[0] << " vs " << names[k] << " bin " << i;
        }
    }
}

}  // namespace
