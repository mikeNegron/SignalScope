#pragma once
// Pure WebSocket framing helpers shared between main.cpp's simple-WS server
// path and the backend tests. No socket I/O lives here - that stays in
// main.cpp where it's bound up with the rest of the server loop.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace signalscope {

// Base64 encoder used by the WS handshake (Sec-WebSocket-Accept).
inline std::string base64_encode(const unsigned char *data, std::size_t len)
{
    static const char b64_table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(4 * ((len + 2) / 3));
    for (std::size_t i = 0; i < len; i += 3) {
        unsigned int n = (data[i] << 16);
        if (i + 1 < len) n |= (data[i + 1] << 8);
        if (i + 2 < len) n |= data[i + 2];
        out += b64_table[(n >> 18) & 0x3f];
        out += b64_table[(n >> 12) & 0x3f];
        out += (i + 1 < len) ? b64_table[(n >> 6) & 0x3f] : '=';
        out += (i + 2 < len) ? b64_table[n & 0x3f]       : '=';
    }
    return out;
}

// Parse one WebSocket frame from `buf`. Returns:
//   N >  0  -> bytes consumed; opcode + payload populated.
//   -1      -> need more bytes (incomplete frame)
//    0      -> malformed (caller should drop the connection)
// Handles 7/16/64-bit length and masking. Doesn't reassemble fragmented
// frames - every command we accept is a single short JSON string that
// fits inside one frame.
inline int parse_ws_frame(const std::uint8_t *buf, std::size_t avail,
                          std::uint8_t &opcode, std::string &payload)
{
    if (avail < 2) return -1;
    opcode = buf[0] & 0x0F;
    const bool masked = (buf[1] & 0x80) != 0;
    std::size_t plen = buf[1] & 0x7F;
    std::size_t off = 2;
    if (plen == 126) {
        if (avail < 4) return -1;
        plen = (std::size_t(buf[2]) << 8) | std::size_t(buf[3]);
        off = 4;
    } else if (plen == 127) {
        if (avail < 10) return -1;
        plen = 0;
        for (int i = 0; i < 8; i++)
            plen = (plen << 8) | std::size_t(buf[2 + i]);
        off = 10;
    }
    std::uint8_t mask[4] = {};
    if (masked) {
        if (avail < off + 4) return -1;
        std::memcpy(mask, buf + off, 4);
        off += 4;
    }
    if (avail < off + plen) return -1;
    payload.assign(plen, '\0');
    for (std::size_t i = 0; i < plen; i++) {
        payload[i] = static_cast<char>(
            buf[off + i] ^ (masked ? mask[i % 4] : 0));
    }
    return static_cast<int>(off + plen);
}

// Build a server->client binary frame header (no mask). Returns the number
// of header bytes written into `header` (which must hold at least 10 bytes).
inline std::size_t build_ws_binary_header(std::uint8_t header[10],
                                          std::size_t payload_len)
{
    header[0] = 0x82; // FIN | binary opcode
    if (payload_len < 126) {
        header[1] = static_cast<std::uint8_t>(payload_len);
        return 2;
    } else if (payload_len < 65536) {
        header[1] = 126;
        header[2] = (payload_len >> 8) & 0xff;
        header[3] = payload_len & 0xff;
        return 4;
    } else {
        header[1] = 127;
        for (int i = 0; i < 8; i++)
            header[2 + i] = (payload_len >> (56 - 8 * i)) & 0xff;
        return 10;
    }
}

} // namespace signalscope
