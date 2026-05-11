// Unit tests for protocol/ws_framing.hpp - pure framing helpers used by the
// simple-WS server fallback. No sockets, no threads, no I/O.

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "protocol/ws_framing.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using signalscope::base64_encode;
using signalscope::build_ws_binary_header;
using signalscope::parse_ws_frame;

// Helpers
// Build a CLIENT->SERVER frame the way browsers do: masked, with a fixed
// mask we can reason about in assertions.
static std::vector<std::uint8_t> make_masked_frame(
    std::uint8_t opcode, const std::string &payload,
    const std::uint8_t (&mask)[4])
{
    std::vector<std::uint8_t> out;
    out.push_back(0x80 | (opcode & 0x0F)); // FIN | opcode

    const std::size_t plen = payload.size();
    if (plen < 126) {
        out.push_back(0x80 | static_cast<std::uint8_t>(plen)); // MASK | len7
    } else if (plen < 65536) {
        out.push_back(0x80 | 126);
        out.push_back((plen >> 8) & 0xff);
        out.push_back(plen & 0xff);
    } else {
        out.push_back(0x80 | 127);
        for (int i = 0; i < 8; i++)
            out.push_back((plen >> (56 - 8 * i)) & 0xff);
    }
    for (int i = 0; i < 4; i++) out.push_back(mask[i]);
    for (std::size_t i = 0; i < plen; i++)
        out.push_back(static_cast<std::uint8_t>(payload[i]) ^ mask[i % 4]);
    return out;
}

// base64_encode
TEST(Base64, KnownVectors) {
    // RFC 4648 §10
    const auto enc = [](const std::string &s) {
        return base64_encode(reinterpret_cast<const unsigned char*>(s.data()),
                             s.size());
    };
    EXPECT_EQ(enc(""),       "");
    EXPECT_EQ(enc("f"),      "Zg==");
    EXPECT_EQ(enc("fo"),     "Zm8=");
    EXPECT_EQ(enc("foo"),    "Zm9v");
    EXPECT_EQ(enc("foob"),   "Zm9vYg==");
    EXPECT_EQ(enc("fooba"),  "Zm9vYmE=");
    EXPECT_EQ(enc("foobar"), "Zm9vYmFy");
}

TEST(Base64, WebSocketAcceptHash) {
    // The canonical WS handshake example from RFC 6455 §1.3.
    const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
    const std::string magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    // SHA1(key + magic) in raw bytes:
    const unsigned char sha1[] = {
        0xb3, 0x7a, 0x4f, 0x2c, 0xc0, 0x62, 0x4f, 0x16, 0x90, 0xf6,
        0x46, 0x06, 0xcf, 0x38, 0x59, 0x45, 0xb2, 0xbe, 0xc4, 0xea
    };
    EXPECT_EQ(base64_encode(sha1, sizeof sha1),
              "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

// parse_ws_frame: short payloads (length fits in 7 bits)
TEST(ParseWsFrame, ShortMaskedText) {
    const std::uint8_t mask[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    const auto frame = make_masked_frame(0x1, "hello", mask);

    std::uint8_t op;
    std::string payload;
    const int consumed = parse_ws_frame(frame.data(), frame.size(), op, payload);

    EXPECT_EQ(consumed, static_cast<int>(frame.size()));
    EXPECT_EQ(op, 0x1);
    EXPECT_EQ(payload, "hello");
}

TEST(ParseWsFrame, ZeroLengthClose) {
    const std::uint8_t mask[4] = {0x01, 0x02, 0x03, 0x04};
    const auto frame = make_masked_frame(0x8, "", mask);

    std::uint8_t op;
    std::string payload;
    const int consumed = parse_ws_frame(frame.data(), frame.size(), op, payload);

    EXPECT_EQ(consumed, static_cast<int>(frame.size()));
    EXPECT_EQ(op, 0x8);
    EXPECT_TRUE(payload.empty());
}

TEST(ParseWsFrame, JsonCommandRoundtrip) {
    const std::string cmd = R"({"cmd":"set_tune_offset","value":1500})";
    const std::uint8_t mask[4] = {0x42, 0x42, 0x42, 0x42};
    const auto frame = make_masked_frame(0x1, cmd, mask);

    std::uint8_t op;
    std::string payload;
    EXPECT_GT(parse_ws_frame(frame.data(), frame.size(), op, payload), 0);
    EXPECT_EQ(op, 0x1);
    EXPECT_EQ(payload, cmd);
}

// parse_ws_frame: 16-bit extended length
TEST(ParseWsFrame, ExtendedLength16Bit) {
    // Anything 126..65535 takes the 2-byte length encoding.
    const std::string payload(300, 'X');
    const std::uint8_t mask[4] = {0x10, 0x20, 0x30, 0x40};
    const auto frame = make_masked_frame(0x1, payload, mask);

    std::uint8_t op;
    std::string out;
    EXPECT_GT(parse_ws_frame(frame.data(), frame.size(), op, out), 0);
    EXPECT_EQ(op, 0x1);
    EXPECT_EQ(out, payload);
}

// parse_ws_frame: incomplete frames
TEST(ParseWsFrame, IncompleteHeaderReturnsNeedMore) {
    // 1 byte - can't even read length yet.
    const std::uint8_t buf[1] = {0x81};
    std::uint8_t op;
    std::string out;
    EXPECT_EQ(parse_ws_frame(buf, sizeof buf, op, out), -1);
}

TEST(ParseWsFrame, IncompleteExtendedLengthReturnsNeedMore) {
    // Says length=126 but only 3 bytes available - need 4 for the 16-bit
    // length field.
    const std::uint8_t buf[3] = {0x81, 0xFE, 0x00};
    std::uint8_t op;
    std::string out;
    EXPECT_EQ(parse_ws_frame(buf, sizeof buf, op, out), -1);
}

TEST(ParseWsFrame, IncompletePayloadReturnsNeedMore) {
    // Header says payload=10, mask present, but only 5 payload bytes given.
    const std::uint8_t mask[4] = {0x01, 0x02, 0x03, 0x04};
    const auto frame = make_masked_frame(0x1, "0123456789", mask);
    std::uint8_t op;
    std::string out;
    // Truncate to header + 5 payload bytes
    EXPECT_EQ(parse_ws_frame(frame.data(), frame.size() - 5, op, out), -1);
}

TEST(ParseWsFrame, PartialThenCompleteParses) {
    // Streaming case: receive 4 bytes, then the rest.
    const std::uint8_t mask[4] = {0xA1, 0xB2, 0xC3, 0xD4};
    const auto frame = make_masked_frame(0x1, "stream", mask);

    std::uint8_t op;
    std::string out;
    EXPECT_EQ(parse_ws_frame(frame.data(), 4, op, out), -1);

    const int consumed = parse_ws_frame(frame.data(), frame.size(), op, out);
    EXPECT_EQ(consumed, static_cast<int>(frame.size()));
    EXPECT_EQ(op, 0x1);
    EXPECT_EQ(out, "stream");
}

// parse_ws_frame: multiple frames concatenated
TEST(ParseWsFrame, BackToBackFramesParseSequentially) {
    const std::uint8_t mask[4] = {0x55, 0x55, 0x55, 0x55};
    auto a = make_masked_frame(0x1, "first",  mask);
    auto b = make_masked_frame(0x1, "second", mask);
    std::vector<std::uint8_t> buf;
    buf.insert(buf.end(), a.begin(), a.end());
    buf.insert(buf.end(), b.begin(), b.end());

    std::uint8_t op;
    std::string out;
    const int n1 = parse_ws_frame(buf.data(), buf.size(), op, out);
    EXPECT_EQ(n1, static_cast<int>(a.size()));
    EXPECT_EQ(out, "first");

    const int n2 = parse_ws_frame(buf.data() + n1, buf.size() - n1, op, out);
    EXPECT_EQ(n2, static_cast<int>(b.size()));
    EXPECT_EQ(out, "second");
}

// build_ws_binary_header
TEST(BuildWsBinaryHeader, ShortPayloadEmitsTwoByteHeader) {
    std::uint8_t hdr[10];
    EXPECT_EQ(build_ws_binary_header(hdr, 64), 2u);
    EXPECT_EQ(hdr[0], 0x82);                // FIN | binary
    EXPECT_EQ(hdr[1], 64);                  // length, no MASK bit
}

TEST(BuildWsBinaryHeader, MediumPayloadEmitsFourByteHeader) {
    std::uint8_t hdr[10];
    EXPECT_EQ(build_ws_binary_header(hdr, 1024), 4u);
    EXPECT_EQ(hdr[0], 0x82);
    EXPECT_EQ(hdr[1], 126);
    EXPECT_EQ((std::size_t(hdr[2]) << 8) | hdr[3], 1024u);
}

TEST(BuildWsBinaryHeader, LargePayloadEmitsTenByteHeader) {
    std::uint8_t hdr[10];
    EXPECT_EQ(build_ws_binary_header(hdr, 1 << 20), 10u);
    EXPECT_EQ(hdr[0], 0x82);
    EXPECT_EQ(hdr[1], 127);
    std::size_t reconstructed = 0;
    for (int i = 0; i < 8; i++) reconstructed = (reconstructed << 8) | hdr[2 + i];
    EXPECT_EQ(reconstructed, 1u << 20);
}

// Round-trip: build + parse
// Rebuilds the server's send path and verifies the parser sees what we sent.

TEST(WsFraming, ServerHeaderUnframesAtClient) {
    std::uint8_t hdr[10];
    const std::string payload = "round-trip";
    const std::size_t hlen = build_ws_binary_header(hdr, payload.size());

    std::vector<std::uint8_t> wire(hdr, hdr + hlen);
    wire.insert(wire.end(), payload.begin(), payload.end());

    std::uint8_t op;
    std::string out;
    const int consumed = parse_ws_frame(wire.data(), wire.size(), op, out);
    EXPECT_EQ(consumed, static_cast<int>(wire.size()));
    EXPECT_EQ(op, 0x2); // binary
    EXPECT_EQ(out, payload);
}
