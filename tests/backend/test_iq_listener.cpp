// IqListenerSource tests.
// Two layers:
//   1) Pure-function unit tests for the wire-format parser and the
//      format-specific sample decoders. No socket, no thread.
//   2) One integration test that opens a real loopback socket on an
//      ephemeral port, pushes a small header+payload through it, and
//      verifies the decoded floats land in a SPSCRingBuffer.

#include <gtest/gtest.h>

#include "source/iq_listener.hpp"
#include "util/ring_buffer.hpp"

#include <arpa/inet.h>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using signalscope::IqListenerSource;
using signalscope::SPSCRingBuffer;
using Format = IqListenerSource::Format;
using State  = IqListenerSource::State;

// Header parser
TEST(IqListenerHeader, RejectsWrongMagic) {
    uint8_t h[16] = { 'X','X','X','X', 0,0,0,0, 0,0,0,0, 0, 0,0,0 };
    auto p = IqListenerSource::parse_header(h);
    EXPECT_FALSE(p.magic_ok);
    EXPECT_FALSE(p.valid);
}

TEST(IqListenerHeader, AcceptsValidMagicAndReadsFields) {
    uint8_t h[16];
    h[0]='S'; h[1]='S'; h[2]='0'; h[3]='1';
    // sample_rate = 2,000,000 Hz, little-endian
    const uint32_t sr = 2'000'000;
    h[4] = sr & 0xFF; h[5] = (sr>>8)&0xFF; h[6] = (sr>>16)&0xFF; h[7] = (sr>>24)&0xFF;
    // center_freq = 100,000,000 Hz
    const uint32_t cf = 100'000'000;
    h[8]  = cf & 0xFF;     h[9]  = (cf>>8)&0xFF;
    h[10] = (cf>>16)&0xFF; h[11] = (cf>>24)&0xFF;
    h[12] = static_cast<uint8_t>(Format::F32IQ);
    h[13] = h[14] = h[15] = 0;

    auto p = IqListenerSource::parse_header(h);
    EXPECT_TRUE(p.magic_ok);
    EXPECT_TRUE(p.valid);
    EXPECT_TRUE(p.sample_rate_present);
    EXPECT_TRUE(p.center_freq_present);
    EXPECT_TRUE(p.format_present);
    EXPECT_EQ(p.sample_rate, sr);
    EXPECT_EQ(p.center_freq, cf);
    EXPECT_EQ(p.format, Format::F32IQ);
}

TEST(IqListenerHeader, SentinelMeansUseDefault) {
    uint8_t h[16];
    h[0]='S'; h[1]='S'; h[2]='0'; h[3]='1';
    // All sentinels: 0xFFFFFFFF for u32, 0xFF for u8.
    for (int i = 4; i < 13; i++) h[i] = 0xFF;
    h[13] = h[14] = h[15] = 0;

    auto p = IqListenerSource::parse_header(h);
    EXPECT_TRUE(p.magic_ok);
    EXPECT_FALSE(p.sample_rate_present);
    EXPECT_FALSE(p.center_freq_present);
    EXPECT_FALSE(p.format_present);
}

TEST(IqListenerHeader, NullPointerIsRejectedSafely) {
    auto p = IqListenerSource::parse_header(nullptr);
    EXPECT_FALSE(p.valid);
}

// Format decoders
TEST(IqListenerDecode, U8IqMapsOffsetBinaryToUnitRange) {
    // RTL-SDR style: byte 127.5 = zero, 0/255 = ±1.
    std::vector<uint8_t> in = {
        128, 128,        // ≈ (0, 0)
        255, 0,          // ≈ (+1, -1)
        64,  192,        // ≈ (-0.5, +0.5)
    };
    std::vector<float> out;
    const std::size_t consumed = IqListenerSource::decode_samples(
        Format::U8IQ, in.data(), in.size(), out);
    EXPECT_EQ(consumed, in.size());
    ASSERT_EQ(out.size(), 6u);
    EXPECT_NEAR(out[0],  0.5f / 127.5f, 1e-5f);   // (128-127.5)/127.5
    EXPECT_NEAR(out[2],  1.0f, 0.01f);            // (255-127.5)/127.5
    EXPECT_NEAR(out[3], -1.0f, 0.01f);            // (0-127.5)/127.5
    EXPECT_NEAR(out[4], (64 - 127.5f) / 127.5f, 1e-5f);
}

TEST(IqListenerDecode, I16IqMapsToUnitRange) {
    // i16 little-endian. Build pairs (I=10000, Q=-10000) and (I=32767, Q=-32768).
    auto put_le16 = [](std::vector<uint8_t>& v, int16_t s) {
        v.push_back(static_cast<uint8_t>(s & 0xFF));
        v.push_back(static_cast<uint8_t>((s >> 8) & 0xFF));
    };
    std::vector<uint8_t> in;
    put_le16(in,  10000); put_le16(in, -10000);
    put_le16(in,  32767); put_le16(in, -32768);

    std::vector<float> out;
    IqListenerSource::decode_samples(Format::I16IQ, in.data(), in.size(), out);
    ASSERT_EQ(out.size(), 4u);
    EXPECT_NEAR(out[0],  10000.0f / 32768.0f, 1e-5f);
    EXPECT_NEAR(out[1], -10000.0f / 32768.0f, 1e-5f);
    EXPECT_NEAR(out[2],  32767.0f / 32768.0f, 1e-4f);
    EXPECT_NEAR(out[3], -1.0f, 1e-5f);
}

TEST(IqListenerDecode, F32IqIsCopiedDirectly) {
    const float pairs[] = {0.25f, -0.5f, 1.0f, 0.0f};
    std::vector<uint8_t> in(sizeof(pairs));
    std::memcpy(in.data(), pairs, sizeof(pairs));

    std::vector<float> out;
    IqListenerSource::decode_samples(Format::F32IQ, in.data(), in.size(), out);
    ASSERT_EQ(out.size(), 4u);
    for (std::size_t i = 0; i < 4; i++) EXPECT_FLOAT_EQ(out[i], pairs[i]);
}

TEST(IqListenerDecode, F32RealIsCopiedDirectly) {
    const float samples[] = {0.1f, -0.2f, 0.3f};
    std::vector<uint8_t> in(sizeof(samples));
    std::memcpy(in.data(), samples, sizeof(samples));

    std::vector<float> out;
    IqListenerSource::decode_samples(Format::F32Real, in.data(), in.size(), out);
    ASSERT_EQ(out.size(), 3u);
    for (std::size_t i = 0; i < 3; i++) EXPECT_FLOAT_EQ(out[i], samples[i]);
}

TEST(IqListenerDecode, ReturnsConsumedTruncatedToWholeSamples) {
    // One trailing byte that doesn't complete a sample must be left for
    // the caller to glue with the next chunk.
    std::vector<uint8_t> in(7, 0);  // 7 bytes; F32Real width = 4; whole = 4
    std::vector<float> out;
    const std::size_t consumed = IqListenerSource::decode_samples(
        Format::F32Real, in.data(), in.size(), out);
    EXPECT_EQ(consumed, 4u);
    EXPECT_EQ(out.size(), 1u);
}

TEST(IqListenerDecode, SampleWidthBytesMatchesFormat) {
    EXPECT_EQ(IqListenerSource::sample_width_bytes(Format::U8IQ),    2u);
    EXPECT_EQ(IqListenerSource::sample_width_bytes(Format::I16IQ),   4u);
    EXPECT_EQ(IqListenerSource::sample_width_bytes(Format::F32IQ),   8u);
    EXPECT_EQ(IqListenerSource::sample_width_bytes(Format::F32Real), 4u);
}

// Integration: real socket round-trip
namespace {

// Tiny helper: connect to (127.0.0.1, port), return the client fd or -1.
int connect_loopback(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool send_all(int fd, const void* buf, std::size_t n) {
    const auto* p = static_cast<const uint8_t*>(buf);
    while (n > 0) {
        const ssize_t k = ::send(fd, p, n, 0);
        if (k <= 0) return false;
        p += k; n -= static_cast<std::size_t>(k);
    }
    return true;
}

} // namespace

TEST(IqListenerIntegration, HeaderAndSamplesRoundTripIntoRingBuffer) {
    SPSCRingBuffer<float> ring(1 << 16);
    IqListenerSource listener;

    // Bind to OS-assigned port (0) so concurrent test runs don't collide.
    IqListenerSource::Defaults defaults;
    defaults.expect_header = true;
    defaults.format        = Format::F32IQ;
    defaults.sample_rate   = 1;
    defaults.center_freq   = 0;
    ASSERT_TRUE(listener.start(0, defaults, ring));
    const uint16_t port = listener.port();
    ASSERT_GT(port, 0);

    // Wait for the listener thread to reach Listening.
    for (int i = 0; i < 100; i++) {
        if (listener.state() == State::Listening) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(listener.state(), State::Listening);

    const int cli = connect_loopback(port);
    ASSERT_GE(cli, 0);

    // Send a header that overrides sr to 4_000_000 and format to I16IQ.
    // Center freq stays as sentinel -> use default (0).
    uint8_t hdr[16] = {0};
    hdr[0]='S'; hdr[1]='S'; hdr[2]='0'; hdr[3]='1';
    const uint32_t sr = 4'000'000;
    hdr[4]=sr&0xFF; hdr[5]=(sr>>8)&0xFF; hdr[6]=(sr>>16)&0xFF; hdr[7]=(sr>>24)&0xFF;
    hdr[8]=hdr[9]=hdr[10]=hdr[11]=0xFF;          // cf sentinel
    hdr[12] = static_cast<uint8_t>(Format::I16IQ);
    ASSERT_TRUE(send_all(cli, hdr, sizeof(hdr)));

    // Send 4 IQ pairs of int16: (1000,-1000) (2000,-2000) (3000,-3000) (4000,-4000).
    int16_t pairs[8] = { 1000,-1000, 2000,-2000, 3000,-3000, 4000,-4000 };
    ASSERT_TRUE(send_all(cli, pairs, sizeof(pairs)));

    // Wait for the listener to land effective values + drain the bytes.
    std::vector<float> got(8);
    std::size_t got_n = 0;
    for (int i = 0; i < 200; i++) {
        got_n = ring.read(got.data() + got_n, got.size() - got_n) + got_n;
        if (got_n >= 8) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(got_n, 8u);
    EXPECT_NEAR(got[0],  1000.0f / 32768.0f, 1e-5f);
    EXPECT_NEAR(got[1], -1000.0f / 32768.0f, 1e-5f);
    EXPECT_NEAR(got[6],  4000.0f / 32768.0f, 1e-5f);
    EXPECT_NEAR(got[7], -4000.0f / 32768.0f, 1e-5f);

    // Effective parameters reflect the header.
    EXPECT_EQ(listener.effective_sample_rate(), sr);
    EXPECT_EQ(listener.effective_format(), Format::I16IQ);
    EXPECT_TRUE(listener.is_complex());

    ::close(cli);
    listener.stop();
    EXPECT_EQ(listener.state(), State::Stopped);
}

TEST(IqListenerIntegration, NoHeaderModeUsesDefaults) {
    SPSCRingBuffer<float> ring(1 << 16);
    IqListenerSource listener;
    IqListenerSource::Defaults defaults;
    defaults.expect_header = false;
    defaults.format        = Format::F32Real;
    defaults.sample_rate   = 48000;
    defaults.center_freq   = 0;
    ASSERT_TRUE(listener.start(0, defaults, ring));

    for (int i = 0; i < 100 && listener.state() != State::Listening; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const int cli = connect_loopback(listener.port());
    ASSERT_GE(cli, 0);

    // Push 3 raw f32 real samples - no header.
    const float samples[] = { 0.1f, 0.5f, -0.25f };
    ASSERT_TRUE(send_all(cli, samples, sizeof(samples)));

    float out[3] = {0};
    std::size_t got = 0;
    for (int i = 0; i < 200; i++) {
        got = ring.read(out + got, 3 - got) + got;
        if (got >= 3) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(got, 3u);
    EXPECT_FLOAT_EQ(out[0],  0.1f);
    EXPECT_FLOAT_EQ(out[1],  0.5f);
    EXPECT_FLOAT_EQ(out[2], -0.25f);
    EXPECT_FALSE(listener.is_complex());
    ::close(cli);
    listener.stop();
}
