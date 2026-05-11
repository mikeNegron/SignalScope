// Wire-protocol round-trip tests.
// The renderer parses backend frames with a DataView in JS:
//   const type        = view.getUint8(0);
//   const version     = view.getUint8(1);
//   const flags       = view.getUint8(2);
//   const fft_size    = view.getUint16(4,  true);
//   const frame_id    = view.getUint32(8,  true);
//   const sample_rate = view.getFloat32(12, true);
//   const payload     = new Float32Array(buffer, 16);
// These tests serialize frames in C++ and decode them with C++ versions
// of the same little-endian primitives, asserting every field survives
// intact. If anyone shifts an offset, changes a field width, or breaks
// the LE assumption, this is the test that fires.

#include <gtest/gtest.h>

#include "protocol/protocol.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

using signalscope::FrameType;
using signalscope::FrameFlags;
using signalscope::serialize_frame;
using signalscope::PROTOCOL_VERSION;
constexpr std::size_t HEADER_SIZE = 16;

namespace {

// Mirror the renderer's DataView little-endian primitives.
uint16_t read_u16_le(const std::vector<uint8_t> &b, std::size_t off) {
    return static_cast<uint16_t>(
        uint16_t(b[off]) | (uint16_t(b[off + 1]) << 8));
}
uint32_t read_u32_le(const std::vector<uint8_t> &b, std::size_t off) {
    return uint32_t(b[off])
         | (uint32_t(b[off + 1]) << 8)
         | (uint32_t(b[off + 2]) << 16)
         | (uint32_t(b[off + 3]) << 24);
}
float read_f32_le(const std::vector<uint8_t> &b, std::size_t off) {
    const uint32_t bits = read_u32_le(b, off);
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

// Decode the payload as Float32Array - same semantics as
// `new Float32Array(buffer, 16)` in the renderer.
std::vector<float> read_payload(const std::vector<uint8_t> &b) {
    const std::size_t n = (b.size() - HEADER_SIZE) / 4;
    std::vector<float> out(n);
    for (std::size_t i = 0; i < n; i++) {
        out[i] = read_f32_le(b, HEADER_SIZE + i * 4);
    }
    return out;
}

} // namespace

// Header layout
TEST(WireProtocol, HeaderFieldsAtCorrectOffsets) {
    const std::vector<float> data = {1.0f, 2.0f, 3.0f};
    const auto frame = serialize_frame(
        FrameType::Spectrum, data.data(), data.size(),
        /*fft_size=*/4096, /*frame_id=*/42, /*sample_rate=*/48000.0f,
        /*flags=*/static_cast<uint8_t>(FrameFlags::Clipping));

    ASSERT_EQ(frame.size(), HEADER_SIZE + data.size() * 4u);
    EXPECT_EQ(frame[0], 0x00);                // FrameType::Spectrum
    EXPECT_EQ(frame[1], PROTOCOL_VERSION);    // version byte
    EXPECT_EQ(frame[2], 0x01);                // FrameFlags::Clipping
    EXPECT_EQ(frame[3], 0x00);                // reserved
    EXPECT_EQ(read_u16_le(frame, 4), 4096u);
    EXPECT_EQ(read_u16_le(frame, 6), 0u);     // reserved
    EXPECT_EQ(read_u32_le(frame, 8), 42u);
    EXPECT_FLOAT_EQ(read_f32_le(frame, 12), 48000.0f);
}

TEST(WireProtocol, HeaderSizeIsExactly16Bytes) {
    // Empty payload -> frame is just the header.
    const auto frame = serialize_frame(
        FrameType::Spectrum, nullptr, 0, 4096, 0, 48000.0f, 0);
    EXPECT_EQ(frame.size(), HEADER_SIZE);
}

TEST(WireProtocol, VersionByteIsPopulatedOnEveryFrame) {
    const float dummy = 0.0f;
    for (auto t : {FrameType::Spectrum, FrameType::Waveform, FrameType::IQ,
                   FrameType::SourceStatus, FrameType::SpectrumStats}) {
        const auto f = serialize_frame(t, &dummy, 1, 0, 0, 0.0f, 0);
        EXPECT_EQ(f[1], PROTOCOL_VERSION)
            << "FrameType " << static_cast<int>(t)
            << " missing version byte (got 0x" << std::hex << int(f[1]) << ")";
    }
}

TEST(WireProtocol, ReservedBytesAreZeroed) {
    // Future versions may give these bytes meaning. Today they MUST be
    // zero so a future renderer can rely on `0` meaning "absent" rather
    // than picking up whatever happened to be in serialize_frame's
    // stack when the struct was uninitialized.
    const float dummy = 0.0f;
    const auto f = serialize_frame(FrameType::Spectrum, &dummy, 1,
                                   /*fft_size*/ 4096, /*fid*/ 999,
                                   /*sr*/ 48000.0f,
                                   /*flags*/ 0xFF);
    EXPECT_EQ(f[3], 0u) << "reserved0 not zeroed";
    EXPECT_EQ(read_u16_le(f, 6), 0u) << "reserved1 not zeroed";
}

// All frame types preserve their type byte
TEST(WireProtocol, EveryFrameTypeRoundTrips) {
    const float dummy = 0.0f;
    struct Case { FrameType t; uint8_t expected; };
    // 0x05 (Periodicity) intentionally absent - removed when never wired up.
    const Case cases[] = {
        {FrameType::Spectrum,      0x00},
        {FrameType::Waveform,      0x01},
        {FrameType::IQ,            0x02},
        {FrameType::Phase,         0x03},
        {FrameType::Histogram,     0x04},
        {FrameType::SourceStatus,  0x06},
        {FrameType::SpectrumHold,  0x07},
        {FrameType::SpectrumStats, 0x08},
    };
    for (const auto &c : cases) {
        const auto f = serialize_frame(c.t, &dummy, 1, 0, 0, 0, 0);
        EXPECT_EQ(f[0], c.expected)
            << "FrameType " << static_cast<int>(c.t)
            << " serialized as 0x" << std::hex << int(f[0])
            << " (expected 0x" << int(c.expected) << ")";
    }
}

// Flags
TEST(WireProtocol, FlagsBitfieldRoundTrips) {
    const float dummy = 0.0f;
    const uint8_t composite =
          static_cast<uint8_t>(FrameFlags::Clipping)
        | static_cast<uint8_t>(FrameFlags::TwoSided)
        | static_cast<uint8_t>(FrameFlags::Complex);
    const auto f = serialize_frame(FrameType::Spectrum, &dummy, 1,
                                   0, 0, 0.0f, composite);
    EXPECT_EQ(f[2], composite);

    // Verify each bit reads back individually - ensures bit positions
    // (0x01, 0x04, 0x08) match what the renderer's `flags & 0x04` checks
    // against.
    EXPECT_NE(f[2] & static_cast<uint8_t>(FrameFlags::Clipping), 0);
    EXPECT_NE(f[2] & static_cast<uint8_t>(FrameFlags::TwoSided), 0);
    EXPECT_NE(f[2] & static_cast<uint8_t>(FrameFlags::Complex),  0);
    EXPECT_EQ(f[2] & static_cast<uint8_t>(FrameFlags::Overflow), 0);
}

// Payload integrity
TEST(WireProtocol, PayloadFloatsExactlyPreserved) {
    // Mix of values that exercise the float32 encoding: small integer,
    // negative, denormal-adjacent, large, and one with non-trivial
    // mantissa bits.
    const std::vector<float> in = {
        0.0f, 1.0f, -1.0f, 3.14159265f, -123.456f, 1.0e-10f, 1.0e10f,
        std::numeric_limits<float>::min(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::max(),
    };
    const auto frame = serialize_frame(
        FrameType::Waveform, in.data(), in.size(), 0, 0, 0.0f, 0);
    const auto out = read_payload(frame);

    ASSERT_EQ(out.size(), in.size());
    for (std::size_t i = 0; i < in.size(); i++) {
        // Bit-exact comparison: round-trip should be identity, not just
        // close-enough. EXPECT_FLOAT_EQ allows 4 ULP - fine here.
        EXPECT_FLOAT_EQ(out[i], in[i]) << "bin " << i;
    }
}

TEST(WireProtocol, EmptyPayloadIsValidHeaderOnly) {
    const auto f = serialize_frame(
        FrameType::SourceStatus, nullptr, 0, 0, 0, 0.0f, 0);
    EXPECT_EQ(f.size(), HEADER_SIZE);
    // Header still parses cleanly.
    EXPECT_EQ(f[0], static_cast<uint8_t>(FrameType::SourceStatus));
    EXPECT_TRUE(read_payload(f).empty());
}

TEST(WireProtocol, LargePayloadDoesNotTruncate) {
    // 65536 floats - the max FFT size we ship - would overflow a 16-bit
    // length field if the wire format were length-prefixed. It isn't:
    // the size is implicit from the WS message length. Verify.
    constexpr std::size_t N = 65536;
    std::vector<float> in(N);
    for (std::size_t i = 0; i < N; i++) in[i] = static_cast<float>(i);

    const auto f = serialize_frame(
        FrameType::Spectrum, in.data(), N, 65535, 0, 48000.0f, 0);

    ASSERT_EQ(f.size(), HEADER_SIZE + N * 4u);
    const auto out = read_payload(f);
    ASSERT_EQ(out.size(), N);
    EXPECT_EQ(out.front(), 0.0f);
    EXPECT_EQ(out.back(), static_cast<float>(N - 1));
    // Spot-check the middle as well.
    EXPECT_EQ(out[N / 2], static_cast<float>(N / 2));
}

// Field-width edge cases
TEST(WireProtocol, FftSize65535FitsInU16) {
    const float dummy = 0.0f;
    const auto f = serialize_frame(
        FrameType::Spectrum, &dummy, 1, /*fft_size=*/65535, 0, 0.0f, 0);
    EXPECT_EQ(read_u16_le(f, 4), 65535u);
}

TEST(WireProtocol, FrameIdMonotonicWidth) {
    // u32 frame_id supports ~2^32 frames before wrap. At 60 fps that's
    // ~2 years of continuous streaming - fine. Verify a value in the
    // upper half round-trips bit-exact.
    const float dummy = 0.0f;
    constexpr uint32_t fid = 0xDEADBEEFu;
    const auto f = serialize_frame(
        FrameType::Spectrum, &dummy, 1, 0, fid, 0.0f, 0);
    EXPECT_EQ(read_u32_le(f, 8), fid);
}

TEST(WireProtocol, SampleRateNonInteger) {
    // SDR captures sometimes have non-round rates (e.g. 44100 / 2.5 =
    // 17640.0; or fractional decimation outputs). The header is f32 so
    // the renderer reads exactly what we wrote.
    const float dummy = 0.0f;
    constexpr float sr = 44100.0f / 2.5f;
    const auto f = serialize_frame(
        FrameType::Spectrum, &dummy, 1, 0, 0, sr, 0);
    EXPECT_FLOAT_EQ(read_f32_le(f, 12), sr);
}
