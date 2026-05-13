#pragma once

#include "fft_backend.hpp"

namespace ss::fft {

// KFR (https://github.com/kfrlib/kfr) single-precision FFT backend. Only
// compiled when CMake finds KFR headers + library. KFR-free is GPL v2;
// linking forces the resulting binary to GPL v2, incompatible with the
// project's MIT distribution. Off by default.
std::unique_ptr<IFftBackend> make_kfr_backend();

}  // namespace ss::fft
