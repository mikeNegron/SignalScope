#pragma once
// IqListenerSource - a TCP server that accepts a single publisher and pumps
// decoded IQ (or real) samples into AppState's capture ring buffer.
// Wire format (16-byte header, sent once per connection - optional):
//   bytes 0-3:   magic         "SS01" (0x53 0x53 0x30 0x31)
//   bytes 4-7:   sample_rate   u32 LE Hz  (0xFFFFFFFF -> use user default)
//   bytes 8-11:  center_freq   u32 LE Hz  (0xFFFFFFFF -> use user default;
//                                          max ~4.29 GHz - beyond that,
//                                          send sentinel and override in UI)
//   byte 12:     format        u8 (0=U8IQ, 1=I16IQ, 2=F32IQ, 3=F32Real,
//                                  0xFF -> use user default)
//   bytes 13-15: reserved      must be zero
// After the header, the publisher streams raw samples in the declared
// format. Real formats push float samples directly; IQ formats push
// interleaved I,Q,I,Q,... matching the same pattern FileSource uses.
// All multi-byte sample formats on the wire are LITTLE-ENDIAN:
//   - I16IQ:   two's-complement int16 LE pairs
//   - F32IQ:   IEEE 754 binary32 LE pairs
//   - F32Real: IEEE 754 binary32 LE
// This matches what every common SDR toolchain (HackRF, GNU Radio,
// SoapySDR, rtl_sdr) emits. We decode through explicit byte-by-byte
// readers so the listener stays correct on a hypothetical big-endian
// host instead of silently producing garbage samples.
// When `expect_header` is false, the listener skips header parsing and
// applies the user-supplied defaults to the byte stream from the first
// byte. This is the "raw stream" path: `hackrf_transfer | nc localhost N`.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include "util/ring_buffer.hpp"

namespace signalscope {

class IqListenerSource {
public:
    enum class Format : uint8_t {
        U8IQ    = 0,    // RTL-SDR style (offset binary, 127.5 = zero)
        I16IQ   = 1,    // Most SDRs (Airspy, BladeRF native, raw int16)
        F32IQ   = 2,    // HackRF native, GNU Radio float complex
        F32Real = 3,    // Real-valued audio-style stream
    };

    enum class State : int {
        Stopped     = 0,
        Listening   = 1,
        Connected   = 2,
        Error       = 3,
    };

    static constexpr uint32_t SENTINEL_U32 = 0xFFFFFFFFu;
    static constexpr uint8_t  SENTINEL_U8  = 0xFFu;
    static constexpr std::size_t HEADER_SIZE = 16;

    struct Defaults {
        uint32_t sample_rate   = 48000;
        uint32_t center_freq   = 0;
        Format   format        = Format::F32IQ;
        bool     expect_header = true;
    };

    IqListenerSource() = default;
    ~IqListenerSource() { stop(); }

    IqListenerSource(const IqListenerSource&) = delete;
    IqListenerSource& operator=(const IqListenerSource&) = delete;

    // Start the listener. Spawns a worker thread that owns the listen socket
    // and (eventually) one client socket. Returns false if bind/listen fails.
    bool start(uint16_t port,
               Defaults defaults,
               SPSCRingBuffer<float>& capture_buffer)
    {
        stop();
        port_  = port;
        {
            std::lock_guard<std::mutex> g(defaults_mutex_);
            defaults_ = defaults;
        }
        capture_buffer_ = &capture_buffer;
        if (!open_listen_socket()) {
            state_.store(State::Error, std::memory_order_release);
            return false;
        }
        running_.store(true, std::memory_order_release);
        state_.store(State::Listening, std::memory_order_release);
        thread_ = std::thread([this] { worker(); });
        return true;
    }

    void stop() {
        running_.store(false, std::memory_order_release);
        // Closing the listen socket pops accept() out of its blocking call.
        if (listen_fd_ >= 0) { ::shutdown(listen_fd_, SHUT_RDWR); ::close(listen_fd_); listen_fd_ = -1; }
        if (client_fd_ >= 0) { ::shutdown(client_fd_, SHUT_RDWR); ::close(client_fd_); client_fd_ = -1; }
        if (thread_.joinable()) thread_.join();
        capture_buffer_ = nullptr;
        state_.store(State::Stopped, std::memory_order_release);
    }

    State state() const noexcept { return state_.load(std::memory_order_acquire); }
    uint16_t port() const noexcept { return port_; }

    // Last-applied effective parameters (header value when present, else
    // the user's default). Reflect the live connection.
    uint32_t effective_sample_rate() const noexcept { return eff_sr_.load(std::memory_order_acquire); }
    uint32_t effective_center_freq() const noexcept { return eff_cf_.load(std::memory_order_acquire); }
    Format   effective_format()      const noexcept { return static_cast<Format>(eff_fmt_.load(std::memory_order_acquire)); }
    bool     is_complex()            const noexcept { return effective_format() != Format::F32Real; }

    std::string last_error() const {
        std::lock_guard<std::mutex> g(err_mutex_);
        return last_err_;
    }

    // Hot-update defaults. Takes effect on the *next* connection (or
    // immediately if `expect_header` was off and no header was read on
    // the current connection - but that's racy enough we don't promise it).
    void update_defaults(Defaults d) {
        std::lock_guard<std::mutex> g(defaults_mutex_);
        defaults_ = d;
    }

    // Pure helpers (exposed for testing)
    struct ParsedHeader {
        bool     valid;
        bool     magic_ok;
        uint32_t sample_rate;       // 0 if sentinel/missing
        uint32_t center_freq;       // 0 if sentinel/missing
        Format   format;
        bool     sample_rate_present;
        bool     center_freq_present;
        bool     format_present;
    };

    // Decode 16 bytes of (potential) header. Magic check is the only
    // way to tell header-vs-data apart when expect_header is true; the
    // caller decides what to do with `valid`/`magic_ok`.
    static ParsedHeader parse_header(const uint8_t* h) noexcept {
        ParsedHeader r{};
        if (!h) return r;
        const bool magic_ok = h[0] == 'S' && h[1] == 'S' && h[2] == '0' && h[3] == '1';
        r.magic_ok = magic_ok;
        r.valid    = magic_ok;
        if (!magic_ok) return r;

        const uint32_t sr = read_u32_le(h + 4);
        const uint32_t cf = read_u32_le(h + 8);
        const uint8_t  fmt = h[12];

        r.sample_rate_present = (sr != SENTINEL_U32);
        r.center_freq_present = (cf != SENTINEL_U32);
        r.format_present      = (fmt != SENTINEL_U8);
        r.sample_rate         = r.sample_rate_present ? sr : 0;
        r.center_freq         = r.center_freq_present ? cf : 0;
        r.format              = r.format_present
            ? static_cast<Format>(std::min<uint8_t>(fmt, static_cast<uint8_t>(Format::F32Real)))
            : Format::F32IQ;
        return r;
    }

    // Decode one chunk of raw bytes as `fmt` and append the float result
    // to `out`. For IQ formats, `out` ends up holding interleaved I,Q,I,Q;
    // for F32Real, just sequential samples. Returns the number of input
    // bytes consumed (a multiple of the per-sample width).
    static std::size_t decode_samples(Format fmt,
                                      const uint8_t* in, std::size_t in_bytes,
                                      std::vector<float>& out)
    {
        const std::size_t width = sample_width_bytes(fmt);
        if (width == 0) return 0;
        const std::size_t whole = (in_bytes / width) * width;
        switch (fmt) {
            case Format::U8IQ: {
                // Offset binary: 127.5 = zero. Maps [0, 255] -> [-1, +1]
                // (with the same scaling rtl_sdr/SDR# use).
                out.reserve(out.size() + whole);
                constexpr float scale = 1.0f / 127.5f;
                for (std::size_t i = 0; i + 1 < whole; i += 2) {
                    out.push_back((static_cast<float>(in[i])     - 127.5f) * scale);
                    out.push_back((static_cast<float>(in[i + 1]) - 127.5f) * scale);
                }
                break;
            }
            case Format::I16IQ: {
                out.reserve(out.size() + whole / 2);
                constexpr float scale = 1.0f / 32768.0f;
                for (std::size_t i = 0; i + 3 < whole; i += 4) {
                    const int16_t i_s = static_cast<int16_t>(read_u16_le(in + i));
                    const int16_t q_s = static_cast<int16_t>(read_u16_le(in + i + 2));
                    out.push_back(static_cast<float>(i_s) * scale);
                    out.push_back(static_cast<float>(q_s) * scale);
                }
                break;
            }
            case Format::F32IQ: {
                // Wire format is IEEE 754 LE pairs; decode explicitly
                // so a BE host doesn't byte-swap floats by accident.
                out.reserve(out.size() + whole / 4);
                for (std::size_t i = 0; i + 3 < whole; i += 4) {
                    out.push_back(read_f32_le(in + i));
                }
                break;
            }
            case Format::F32Real: {
                out.reserve(out.size() + whole / 4);
                for (std::size_t i = 0; i + 3 < whole; i += 4) {
                    out.push_back(read_f32_le(in + i));
                }
                break;
            }
        }
        return whole;
    }

    static std::size_t sample_width_bytes(Format fmt) noexcept {
        switch (fmt) {
            case Format::U8IQ:    return 2;  // 2 × u8
            case Format::I16IQ:   return 4;  // 2 × i16
            case Format::F32IQ:   return 8;  // 2 × f32
            case Format::F32Real: return 4;  // 1 × f32
        }
        return 0;
    }

    static Format format_from_int(int v) noexcept {
        switch (v) {
            case 0: return Format::U8IQ;
            case 1: return Format::I16IQ;
            case 2: return Format::F32IQ;
            case 3: return Format::F32Real;
            default: return Format::F32IQ;
        }
    }

private:
    bool open_listen_socket() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) { set_error("socket() failed"); return false; }

        int yes = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 127.0.0.1 only
        addr.sin_port        = htons(port_);
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            set_error(std::string("bind() failed: ") + std::strerror(errno));
            ::close(listen_fd_); listen_fd_ = -1;
            return false;
        }
        if (::listen(listen_fd_, 1) < 0) {
            set_error(std::string("listen() failed: ") + std::strerror(errno));
            ::close(listen_fd_); listen_fd_ = -1;
            return false;
        }
        // If port_ was 0 the OS picked one - read it back so callers can find us.
        socklen_t alen = sizeof(addr);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &alen) == 0) {
            port_ = ntohs(addr.sin_port);
        }
        return true;
    }

    void worker() {
        while (running_.load(std::memory_order_acquire)) {
            sockaddr_in peer{};
            socklen_t plen = sizeof(peer);
            const int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &plen);
            if (fd < 0) {
                if (!running_.load(std::memory_order_acquire)) break;
                if (errno == EINTR) continue;
                set_error(std::string("accept() failed: ") + std::strerror(errno));
                state_.store(State::Error, std::memory_order_release);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            client_fd_ = fd;
            state_.store(State::Connected, std::memory_order_release);
            handle_client();
            // Returning from handle_client means the publisher is gone. Go
            // back to listening for the next one rather than tearing the
            // listener down; the user can still see the spectrum freeze.
            if (client_fd_ >= 0) { ::close(client_fd_); client_fd_ = -1; }
            if (running_.load(std::memory_order_acquire)) {
                state_.store(State::Listening, std::memory_order_release);
            }
        }
    }

    void handle_client() {
        // Snapshot defaults so a hot update mid-stream doesn't mid-corrupt
        // the in-flight format negotiation.
        Defaults snap;
        {
            std::lock_guard<std::mutex> g(defaults_mutex_);
            snap = defaults_;
        }

        Format   active_fmt = snap.format;
        uint32_t active_sr  = snap.sample_rate;
        uint32_t active_cf  = snap.center_freq;

        if (snap.expect_header) {
            uint8_t hdr[HEADER_SIZE];
            if (!read_exact(hdr, HEADER_SIZE)) return;
            const ParsedHeader p = parse_header(hdr);
            if (!p.magic_ok) {
                set_error("header magic mismatch (expected 'SS01')");
                return;
            }
            if (p.sample_rate_present) active_sr  = p.sample_rate;
            if (p.center_freq_present) active_cf  = p.center_freq;
            if (p.format_present)      active_fmt = p.format;
        }

        eff_sr_.store(active_sr,  std::memory_order_release);
        eff_cf_.store(active_cf,  std::memory_order_release);
        eff_fmt_.store(static_cast<int>(active_fmt), std::memory_order_release);

        const std::size_t width = sample_width_bytes(active_fmt);
        if (width == 0) return;

        std::vector<uint8_t> buf(64 * 1024);
        std::vector<uint8_t> leftover;
        leftover.reserve(width);
        std::vector<float> decoded;
        decoded.reserve(buf.size());

        while (running_.load(std::memory_order_acquire)) {
            const ssize_t n = ::recv(client_fd_, buf.data(), buf.size(), 0);
            if (n <= 0) {
                if (n < 0 && errno == EINTR) continue;
                return;  // disconnect / error -> outer loop reaccepts
            }
            // Glue any leftover from the previous read with this chunk so
            // a sample doesn't get split across recv() boundaries.
            std::vector<uint8_t> chunk;
            chunk.reserve(leftover.size() + static_cast<std::size_t>(n));
            chunk.insert(chunk.end(), leftover.begin(), leftover.end());
            chunk.insert(chunk.end(), buf.begin(), buf.begin() + n);

            decoded.clear();
            const std::size_t consumed = decode_samples(
                active_fmt, chunk.data(), chunk.size(), decoded);

            leftover.assign(chunk.begin() + consumed, chunk.end());

            if (capture_buffer_ && !decoded.empty()) {
                // Ring auto-drops oldest on overflow - there's no useful
                // back-pressure path from a TCP receive loop.
                std::ignore = capture_buffer_->write(decoded.data(), decoded.size());
            }
        }
    }

    bool read_exact(uint8_t* dst, std::size_t n) {
        std::size_t got = 0;
        while (got < n) {
            const ssize_t r = ::recv(client_fd_, dst + got, n - got, 0);
            if (r <= 0) {
                if (r < 0 && errno == EINTR) continue;
                return false;
            }
            got += static_cast<std::size_t>(r);
        }
        return true;
    }

    void set_error(std::string msg) {
        std::lock_guard<std::mutex> g(err_mutex_);
        last_err_ = std::move(msg);
    }

    static uint32_t read_u32_le(const uint8_t* p) noexcept {
        return  static_cast<uint32_t>(p[0])
             | (static_cast<uint32_t>(p[1]) << 8)
             | (static_cast<uint32_t>(p[2]) << 16)
             | (static_cast<uint32_t>(p[3]) << 24);
    }
    static uint16_t read_u16_le(const uint8_t* p) noexcept {
        return  static_cast<uint16_t>(p[0])
             | (static_cast<uint16_t>(p[1]) << 8);
    }
    // Reconstruct a float from its LE byte representation. Routes through
    // an explicit u32 reassembly so it stays correct on big-endian hosts
    // (where `memcpy(&f, p, 4)` would byte-swap the float silently).
    static float read_f32_le(const uint8_t* p) noexcept {
        const uint32_t bits = read_u32_le(p);
        float v;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<State> state_{State::Stopped};

    int listen_fd_ = -1;
    int client_fd_ = -1;
    uint16_t port_ = 0;

    std::mutex defaults_mutex_;
    Defaults defaults_;

    std::atomic<uint32_t> eff_sr_{0};
    std::atomic<uint32_t> eff_cf_{0};
    std::atomic<int>      eff_fmt_{static_cast<int>(Format::F32IQ)};

    SPSCRingBuffer<float>* capture_buffer_ = nullptr;

    mutable std::mutex err_mutex_;
    std::string last_err_;
};

} // namespace signalscope
