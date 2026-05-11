#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

namespace signalscope {

// Wire protocol: binary WebSocket messages laid out as
// [FrameHeader (16 B)][float32 payload]. Header is 16 bytes so the
// payload sits at a 4-byte boundary (Float32Array rejects unaligned
// offsets), with reserved space for future fields without a wire bump.
//
// Version byte: bumped on any change to field offset, width, or
// meaning. Renderers MUST validate `version` and drop frames they
// don't understand — the byte is the discriminator for parser branches.

constexpr uint8_t PROTOCOL_VERSION = 0x01;

struct FrameHeader {
    uint8_t  type;          // FrameType enum
    uint8_t  version;       // PROTOCOL_VERSION - see comment above
    uint8_t  flags;          // FrameFlags bitmask
    uint8_t  reserved0;     // must be 0
    uint16_t fft_size;      // FFT size used
    uint16_t reserved1;     // must be 0
    uint32_t frame_id;      // monotonic frame counter
    float    sample_rate;   // current sample rate
    // Total: 16 bytes, followed by float[] payload (4-byte aligned).
};
static_assert(sizeof(FrameHeader) == 16, "FrameHeader must be 16 bytes");

enum class FrameType : uint8_t {
    Spectrum     = 0x00,  // float[N/2 real, or N two-sided] - magnitude in dBFS
    Waveform     = 0x01,  // float[N]   - normalized samples [-1, 1]
    IQ           = 0x02,  // float[N*2] - planar: first N floats are I, next N are Q
    Phase        = 0x03,  // float[N/2 real, or N two-sided] - phase in radians
    Histogram    = 0x04,  // float[256] - amplitude distribution
    // 0x05 was Periodicity - removed when never wired up. Don't reuse
    // until protocol version bumps so an older renderer can't mis-decode.
    SourceStatus = 0x06,  // float[4]   - [loop_count, source_mode, is_complex, center_freq]
    SpectrumHold = 0x07,  // float[N/2 real, or N two-sided] - peak-hold trace (only sent when enabled)
    SpectrumStats= 0x08,  // float[3 + 3*K + 2] - [noise_floor, fundamental_hz,
                          //   peak_count K, K×(bin, value, prominence),
                          //   spec_min_db, spec_max_db]. The trailing min/max
                          //   pair feeds the renderer's Flatten auto-range
                          //   without making it re-iterate the full spectrum
                          //   each frame. Older renderers that don't read
                          //   them just ignore the extra floats.
    HistoryRow   = 0x09,  // float[N] - historical spectrum row, sent in response
                          //            to request_history. The frame's frame_id
                          //            field carries the row's offset from
                          //            newest (0 = newest, increasing back in
                          //            time); fft_size + flags + sample_rate
                          //            reflect that row's capture-time settings.
};

enum class FrameFlags : uint8_t {
    None      = 0x00,
    Clipping  = 0x01,
    Overflow  = 0x02,
    TwoSided  = 0x04, // spectrum/phase span [-fs/2, +fs/2] (length = N) instead of [0, fs/2] (length = N/2)
    Complex   = 0x08, // source is complex/IQ
};

// Command protocol (JS -> C++) — JSON text WebSocket messages of
// the form { "cmd": "<name>", "value": ... }. Handlers and supported
// names live in main.cpp's command_table().

enum class WindowFunction : uint8_t {
    Rectangular = 0,
    Hanning,
    Hamming,
    Blackman,
    FlatTop,
    Kaiser,
    Count
};

// On LE hosts FrameHeader memcpys straight to the wire because the
// field layout already matches the renderer's LE DataView reads.
// `static_assert(sizeof == 16)` above pins the layout.
inline std::vector<uint8_t> serialize_frame(
    FrameType type, const float* data, std::size_t count,
    uint16_t fft_size, uint32_t frame_id, float sample_rate, uint8_t flags)
{
    std::vector<uint8_t> buf(sizeof(FrameHeader) + count * sizeof(float));
    FrameHeader header{};
    header.type        = static_cast<uint8_t>(type);
    header.version     = PROTOCOL_VERSION;
    header.flags       = flags;
    header.fft_size    = fft_size;
    header.frame_id    = frame_id;
    header.sample_rate = sample_rate;
    std::memcpy(buf.data(), &header, sizeof(header));
    if (count > 0 && data != nullptr) {
        std::memcpy(buf.data() + sizeof(header), data, count * sizeof(float));
    }
    return buf;
}

// Map string names to enum
inline WindowFunction window_from_string(const std::string& name) {
    if (name == "hanning")     return WindowFunction::Hanning;
    if (name == "hamming")     return WindowFunction::Hamming;
    if (name == "blackman")    return WindowFunction::Blackman;
    if (name == "flattop")     return WindowFunction::FlatTop;
    if (name == "kaiser")      return WindowFunction::Kaiser;
    if (name == "rectangular") return WindowFunction::Rectangular;
    return WindowFunction::Hanning;  // default
}

} // namespace signalscope
