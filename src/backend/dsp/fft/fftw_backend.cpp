#include "fftw_backend.hpp"

#include <fftw3.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>

namespace ss::fft {

namespace {

// $HOME/.cache/signalscope/fftw-wisdom, or empty if HOME is unset.
std::string wisdom_file_path() {
    const char* home = std::getenv("HOME");
    if (!home || !*home) return {};
    std::filesystem::path p =
        std::filesystem::path(home) / ".cache" / "signalscope" / "fftw-wisdom";
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    return p.string();
}

class FftwBackend final : public IFftBackend {
public:
    ~FftwBackend() override { free_plans(); }

    void resize(std::size_t n) override {
        free_plans();
        n_ = n;

        // Try cached wisdom first (instant if hit), fall back to ESTIMATE
        // so resize never blocks. Matches the legacy behavior in
        // dsp_pipeline before the backend split.
        in_real_  = fftwf_alloc_real(n);
        out_r2c_  = fftwf_alloc_complex(n / 2 + 1);
        plan_r2c_ = fftwf_plan_dft_r2c_1d(
            static_cast<int>(n), in_real_, out_r2c_,
            FFTW_MEASURE | FFTW_WISDOM_ONLY);
        if (!plan_r2c_) {
            plan_r2c_ = fftwf_plan_dft_r2c_1d(
                static_cast<int>(n), in_real_, out_r2c_, FFTW_ESTIMATE);
        }

        in_complex_  = fftwf_alloc_complex(n);
        out_complex_ = fftwf_alloc_complex(n);
        plan_c2c_ = fftwf_plan_dft_1d(
            static_cast<int>(n), in_complex_, out_complex_,
            FFTW_FORWARD, FFTW_MEASURE | FFTW_WISDOM_ONLY);
        if (!plan_c2c_) {
            plan_c2c_ = fftwf_plan_dft_1d(
                static_cast<int>(n), in_complex_, out_complex_,
                FFTW_FORWARD, FFTW_ESTIMATE);
        }
    }

    float* input_real() noexcept override { return in_real_; }
    std::complex<float>* output_r2c() noexcept override {
        return reinterpret_cast<std::complex<float>*>(out_r2c_);
    }
    std::complex<float>* input_complex() noexcept override {
        return reinterpret_cast<std::complex<float>*>(in_complex_);
    }
    std::complex<float>* output_complex() noexcept override {
        return reinterpret_cast<std::complex<float>*>(out_complex_);
    }

    void execute_r2c() noexcept override { fftwf_execute(plan_r2c_); }
    void execute_c2c_forward() noexcept override { fftwf_execute(plan_c2c_); }

    std::string_view name() const noexcept override { return "fftw"; }

private:
    void free_plans() {
        if (plan_r2c_)    { fftwf_destroy_plan(plan_r2c_);  plan_r2c_   = nullptr; }
        if (plan_c2c_)    { fftwf_destroy_plan(plan_c2c_);  plan_c2c_   = nullptr; }
        if (in_real_)     { fftwf_free(in_real_);           in_real_    = nullptr; }
        if (out_r2c_)     { fftwf_free(out_r2c_);           out_r2c_    = nullptr; }
        if (in_complex_)  { fftwf_free(in_complex_);        in_complex_ = nullptr; }
        if (out_complex_) { fftwf_free(out_complex_);       out_complex_= nullptr; }
    }

    std::size_t n_ = 0;
    fftwf_plan    plan_r2c_    = nullptr;
    fftwf_plan    plan_c2c_    = nullptr;
    float*        in_real_     = nullptr;
    fftwf_complex* out_r2c_    = nullptr;
    fftwf_complex* in_complex_ = nullptr;
    fftwf_complex* out_complex_= nullptr;
};

bool g_started = false;
std::string g_wisdom_path;

}  // namespace

std::unique_ptr<IFftBackend> make_fftw_backend() {
    return std::make_unique<FftwBackend>();
}

void fftw_global_startup() {
    if (g_started) return;
    fftwf_make_planner_thread_safe();
    g_wisdom_path = wisdom_file_path();
    if (!g_wisdom_path.empty()) {
        fftwf_import_wisdom_from_filename(g_wisdom_path.c_str());
    }
    g_started = true;
}

void fftw_warmup(std::span<const std::size_t> sizes,
                 bool (*should_cancel)())
{
    auto canceled = [&]() {
        return should_cancel && should_cancel();
    };

    for (std::size_t n : sizes) {
        if (canceled()) return;

        float* in_r = fftwf_alloc_real(n);
        fftwf_complex* out_r = fftwf_alloc_complex(n / 2 + 1);
        fftwf_plan p = fftwf_plan_dft_r2c_1d(
            static_cast<int>(n), in_r, out_r,
            FFTW_MEASURE | FFTW_WISDOM_ONLY);
        if (!p) {
            p = fftwf_plan_dft_r2c_1d(
                static_cast<int>(n), in_r, out_r, FFTW_MEASURE);
            if (p && !g_wisdom_path.empty()) {
                fftwf_export_wisdom_to_filename(g_wisdom_path.c_str());
            }
        }
        if (p) fftwf_destroy_plan(p);
        fftwf_free(in_r);
        fftwf_free(out_r);

        if (canceled()) return;

        fftwf_complex* in_c  = fftwf_alloc_complex(n);
        fftwf_complex* out_c = fftwf_alloc_complex(n);
        fftwf_plan pc = fftwf_plan_dft_1d(
            static_cast<int>(n), in_c, out_c,
            FFTW_FORWARD, FFTW_MEASURE | FFTW_WISDOM_ONLY);
        if (!pc) {
            pc = fftwf_plan_dft_1d(
                static_cast<int>(n), in_c, out_c,
                FFTW_FORWARD, FFTW_MEASURE);
            if (pc && !g_wisdom_path.empty()) {
                fftwf_export_wisdom_to_filename(g_wisdom_path.c_str());
            }
        }
        if (pc) fftwf_destroy_plan(pc);
        fftwf_free(in_c);
        fftwf_free(out_c);
    }
}

void fftw_global_shutdown() {
    if (!g_started) return;
    if (!g_wisdom_path.empty()) {
        fftwf_export_wisdom_to_filename(g_wisdom_path.c_str());
    }
}

}  // namespace ss::fft
