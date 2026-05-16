// Microbench for the DSP per-frame hot loops. Not a gtest binary — runs
// a fixed number of iterations across representative FFT sizes and
// prints per-frame microseconds + samples/sec.
//
// Usage: ./bench_dsp_pipeline [iterations]   (default 5000)
// Output: one line per FFT size, JSON-ish so a follow-up script can diff
// two runs.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <vector>

#include "dsp/dsp_pipeline.hpp"

namespace {

// Deterministic PRNG so two runs see byte-identical input.
struct Xoshiro128 {
    uint32_t s0 = 0x9E3779B9, s1 = 0x243F6A88, s2 = 0xB7E15162, s3 = 0xBB67AE85;
    uint32_t next() {
        const uint32_t r = s0 + s3;
        const uint32_t t = s1 << 9;
        s2 ^= s0; s3 ^= s1; s1 ^= s2; s0 ^= s3;
        s2 ^= t;
        s3 = (s3 << 11) | (s3 >> 21);
        return r;
    }
    float next_unit() {  // [-1, 1)
        return (static_cast<int32_t>(next()) * (1.0f / 2147483648.0f));
    }
};

std::vector<float> make_input(std::size_t n, uint32_t seed) {
    Xoshiro128 rng;
    rng.s0 ^= seed;
    std::vector<float> v(n);
    for (auto& x : v) x = rng.next_unit() * 0.5f;
    return v;
}

// Run process_spectrum `iters` times against pre-generated input and
// return the median per-iteration microseconds.
double bench_real(std::size_t fft_size, std::size_t iters) {
    signalscope::DSPPipeline dsp{{fft_size, 48000.0f, signalscope::WindowFunction::Hanning}};
    const auto input = make_input(fft_size, static_cast<uint32_t>(fft_size));

    // Warm-up so first-call FFT planner cost is excluded.
    for (int i = 0; i < 16; i++) (void) dsp.process_spectrum(input);

    std::vector<double> times_us;
    times_us.reserve(iters);
    for (std::size_t i = 0; i < iters; i++) {
        auto t0 = std::chrono::steady_clock::now();
        const auto& out = dsp.process_spectrum(input);
        auto t1 = std::chrono::steady_clock::now();
        times_us.push_back(
            std::chrono::duration<double, std::micro>(t1 - t0).count());
        // Side-effect to keep `out` live (defeats DCE).
        if (!std::isfinite(out[0])) std::abort();
    }
    std::sort(times_us.begin(), times_us.end());
    return times_us[times_us.size() / 2];
}

// Run process_spectrum_complex `iters` times against pre-generated I/Q
// inputs and return the median per-iteration microseconds. I and Q are
// seeded with distinct constants so the two channels aren't identical
// (which would let the compiler/FFT exploit symmetry).
double bench_complex(std::size_t fft_size, std::size_t iters) {
    signalscope::DSPPipeline dsp{{fft_size, 48000.0f, signalscope::WindowFunction::Hanning}};
    const auto i_input = make_input(fft_size, static_cast<uint32_t>(fft_size));
    const auto q_input = make_input(fft_size,
                                    static_cast<uint32_t>(fft_size) + 0xDEADBEEFu);

    // Warm-up so first-call FFT planner cost is excluded.
    for (int i = 0; i < 16; i++) (void) dsp.process_spectrum_complex(i_input, q_input);

    std::vector<double> times_us;
    times_us.reserve(iters);
    for (std::size_t i = 0; i < iters; i++) {
        auto t0 = std::chrono::steady_clock::now();
        const auto& out = dsp.process_spectrum_complex(i_input, q_input);
        auto t1 = std::chrono::steady_clock::now();
        times_us.push_back(
            std::chrono::duration<double, std::micro>(t1 - t0).count());
        // Side-effect to keep `out` live (defeats DCE).
        if (!std::isfinite(out[0])) std::abort();
    }
    std::sort(times_us.begin(), times_us.end());
    return times_us[times_us.size() / 2];
}

} // namespace

int main(int argc, char** argv) {
    std::size_t iters = (argc > 1) ? std::strtoull(argv[1], nullptr, 10) : 5000;

    const std::size_t sizes[] = {1024, 2048, 4096, 8192, 16384, 32768, 65536};
    std::printf("{\n");
    std::printf("  \"iters\": %zu,\n", iters);
    std::printf("  \"real\": {\n");
    bool first = true;
    for (auto n : sizes) {
        const double us = bench_real(n, iters);
        std::printf("%s    \"%zu\": %.3f", first ? "" : ",\n", n, us);
        first = false;
    }
    std::printf("\n  },\n");
    std::printf("  \"complex\": {\n");
    first = true;
    for (auto n : sizes) {
        const double us = bench_complex(n, iters);
        std::printf("%s    \"%zu\": %.3f", first ? "" : ",\n", n, us);
        first = false;
    }
    std::printf("\n  }\n}\n");
    return 0;
}
