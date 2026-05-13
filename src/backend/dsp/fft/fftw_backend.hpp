#pragma once

#include "fft_backend.hpp"

namespace ss::fft {

// Single-precision FFTW3. Only compiled when CMake finds libfftw3f. GPL v2 —
// linking forces the resulting binary to GPL v2 as well, which is
// incompatible with the project's MIT distribution. Off by default.
std::unique_ptr<IFftBackend> make_fftw_backend();

// FFTW-specific process startup: fftwf_make_planner_thread_safe() and
// wisdom import from $HOME/.cache/signalscope/fftw-wisdom. Idempotent.
void fftw_global_startup();

// Background warmup. For each FFT size in `sizes`, runs FFTW_MEASURE to
// populate wisdom (r2c + c2c) and persists wisdom after each new plan.
// Cooperative cancellation via should_cancel().
void fftw_warmup(std::span<const std::size_t> sizes,
                 bool (*should_cancel)());

// Persists FFTW wisdom to disk. Safe to call from any thread.
void fftw_global_shutdown();

}  // namespace ss::fft
