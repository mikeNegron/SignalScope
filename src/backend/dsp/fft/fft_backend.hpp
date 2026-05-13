#pragma once

// FFT backend abstraction.
//
// pocketfft (BSD-3) is always compiled and is the default. FFTW3 and KFR
// are optional and conditionally compiled when CMake detects them; both
// are GPL v2 and taint the resulting binary's license. See README.

#include <complex>
#include <cstddef>
#include <memory>
#include <span>
#include <string_view>

namespace ss::fft {

// Pure-virtual transform interface. One instance per DSP pipeline (FFTW
// plan execution is not thread-safe across threads, so each consumer owns
// its own buffers and plans).
class IFftBackend {
public:
    virtual ~IFftBackend() = default;

    // (Re)allocate buffers + plans for FFT size n. Pointers returned by
    // input_*/output_* are stable until the next resize().
    virtual void resize(std::size_t n) = 0;

    // Zero-copy buffer access. Caller writes into input_*, calls the
    // matching execute_*, reads from output_*. Layouts:
    //   input_real:      n floats
    //   output_r2c:      n/2 + 1 complex bins (one-sided spectrum)
    //   input_complex:   n complex samples
    //   output_complex:  n complex bins (two-sided spectrum)
    virtual float*               input_real()     noexcept = 0;
    virtual std::complex<float>* output_r2c()     noexcept = 0;
    virtual std::complex<float>* input_complex()  noexcept = 0;
    virtual std::complex<float>* output_complex() noexcept = 0;

    virtual void execute_r2c()         noexcept = 0;
    virtual void execute_c2c_forward() noexcept = 0;

    virtual std::string_view name() const noexcept = 0;
};

enum class BackendKind { Pocketfft, Fftw, Kfr };

// Picks the active backend for this process. Resolution order:
//   1. `preference` argument (e.g. from --fft-backend=)
//   2. SS_FFT_BACKEND env var
//   3. Default: pocketfft
// Falls back to pocketfft (with a warning to stderr) if the requested
// backend wasn't compiled in. Call once at startup, before any pipeline
// constructs a backend.
BackendKind initialize(std::string_view preference = {});

// Reports the kind chosen by initialize(). Defaults to Pocketfft if
// initialize() hasn't been called.
BackendKind active_kind() noexcept;

// Human-readable name of the active backend (matches IFftBackend::name).
std::string_view active_name() noexcept;

// Constructs a new backend instance using the kind chosen by initialize().
std::unique_ptr<IFftBackend> make_backend();

// Backend-specific startup work (e.g. FFTW planner thread-safety, wisdom
// import). No-op for pocketfft. Idempotent.
void global_startup();

// Backend-specific background warmup, e.g. populating FFTW wisdom for
// common FFT sizes. No-op for pocketfft. Safe to call from a detached
// background thread; respect cancellation via the cancel functor (returning
// true aborts the loop).
void warmup(std::span<const std::size_t> sizes,
            bool (*should_cancel)() = nullptr);

// Persist backend-specific state (e.g. FFTW wisdom export). No-op for
// pocketfft. Call at clean shutdown.
void global_shutdown();

}  // namespace ss::fft
