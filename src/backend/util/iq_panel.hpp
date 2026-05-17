#pragma once

#include <algorithm>
#include <cstddef>

namespace signalscope {

/// Compute a safe IQ-panel point count given a requested length and the
/// current hop size.
///
/// The real-source IQ-panel build path strides the hop with index `i*2 + 1`,
/// so the last addressable sample is at `hop_size - 1` and the contract is
/// `iq_len * 2 <= hop_size`. When `hop_size < 2` (very small FFT combined
/// with high overlap), the only safe answer is zero - any positive length
/// would index out of bounds.
///
/// This is the unconditional safety clamp for the WS broadcast hot path
/// (see main.cpp's IQ panel block). Factored out so the edge case is
/// covered by a small unit test (`tests/backend/test_iq_panel.cpp`)
/// without standing up the full WS pipeline.
constexpr std::size_t clamp_iq_len(std::size_t requested,
                                   std::size_t hop_size) noexcept
{
    if (hop_size < 2) return 0;
    return std::min(requested, hop_size / 2);
}

} // namespace signalscope
