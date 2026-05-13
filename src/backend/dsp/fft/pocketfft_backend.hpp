#pragma once

#include "fft_backend.hpp"

namespace ss::fft {

// Header-only pocketfft (BSD-3-Clause). Always compiled. Default backend.
std::unique_ptr<IFftBackend> make_pocketfft_backend();

}  // namespace ss::fft
