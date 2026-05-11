// Integration test for the simple-WS server's multi-client behavior.
// Spawns the backend binary as a subprocess (test signal source) on a
// throwaway port, opens raw client sockets from this process, drives the
// HTTP upgrade by hand, then asserts that:
//   1. Multiple clients can connect concurrently and each receives frames
//      (the regression that motivated this refactor - the old code
//      single-tracked clients).
//   2. Disconnecting one client doesn't disrupt the others.
//   3. A client can reconnect after a disconnect.
// The test treats the backend as a black box; framing is parsed using the
// same helpers the server side uses (protocol/ws_framing.hpp).
// SIGNALSCOPE_BACKEND_BIN is provided by tests/backend/CMakeLists.txt.

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "protocol/ws_framing.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifndef SIGNALSCOPE_BACKEND_BIN
#  error "SIGNALSCOPE_BACKEND_BIN must be defined by the build system"
#endif

using namespace std::chrono_literals;
using signalscope::base64_encode;
using signalscope::parse_ws_frame;

namespace {

// Backend subprocess fixture
class BackendProcess {
public:
    BackendProcess() = default;
    ~BackendProcess() { stop(); }

    // Boots the backend on a random ephemeral port. Returns true once the
    // listen socket is accepting (verified by a probe connect).
    bool start() {
        port_ = pick_port();
        if (port_ == 0) return false;

        const std::string port_arg = "--port=" + std::to_string(port_);
        const char *bin = SIGNALSCOPE_BACKEND_BIN;
        if (!std::filesystem::exists(bin)) {
            std::fprintf(stderr,
                "[test] Backend binary not found at %s\n"
                "       Build it first: npm run build:backend\n"
                "       Or, if testing the simple-WS path:\n"
                "         cmake -B build/backend-simple -S src/backend "
                "-DFORCE_SIMPLE_WS=ON\n"
                "         cmake --build build/backend-simple --config Release\n",
                bin);
            return false;
        }

        pid_ = fork();
        if (pid_ < 0) return false;
        if (pid_ == 0) {
            // Child - exec the backend with a test signal so we get frames
            // without needing a mic.
            execl(bin, bin, "--test", port_arg.c_str(),
                  static_cast<char *>(nullptr));
            std::_Exit(127);
        }

        // Wait for the listen socket to come up (≤ 3 s).
        for (int i = 0; i < 60; i++) {
            int fd = dial(port_, /*timeout_ms*/ 50);
            if (fd >= 0) { ::close(fd); return true; }
            std::this_thread::sleep_for(50ms);
        }
        stop();
        return false;
    }

    void stop() {
        if (pid_ > 0) {
            ::kill(pid_, SIGTERM);
            int status = 0;
            for (int i = 0; i < 40; i++) {
                pid_t r = ::waitpid(pid_, &status, WNOHANG);
                if (r == pid_) { pid_ = -1; return; }
                std::this_thread::sleep_for(25ms);
            }
            ::kill(pid_, SIGKILL);
            ::waitpid(pid_, &status, 0);
            pid_ = -1;
        }
    }

    int port() const { return port_; }

    // Open a raw TCP connection to the backend. Caller owns the fd.
    static int dial(int port, int timeout_ms = 1000) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(static_cast<uint16_t>(port));

        // Use non-blocking + poll so a closed listener fails fast instead
        // of hitting the kernel's connect timeout.
        ::fcntl(fd, F_SETFL, O_NONBLOCK);
        int r = ::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr);
        if (r < 0 && errno != EINPROGRESS) { ::close(fd); return -1; }

        pollfd pfd{fd, POLLOUT, 0};
        if (::poll(&pfd, 1, timeout_ms) <= 0) { ::close(fd); return -1; }
        int err = 0; socklen_t errlen = sizeof err;
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
        if (err != 0) { ::close(fd); return -1; }

        // Restore blocking mode for the test client. Test reads use poll
        // explicitly; writes are small and best-effort blocking.
        int flags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
        return fd;
    }

private:
    pid_t pid_ = -1;
    int   port_ = 0;

    // Reserve an ephemeral port by binding+closing a temporary socket.
    // The kernel's TIME_WAIT cycle means there's a tiny race here, but
    // 0-65535 collisions in a 100 ms window are rare enough that retry
    // logic isn't worth the complexity.
    static int pick_port() {
        int s = ::socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) return 0;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        int opt = 1;
        ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
        if (::bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof addr) < 0) {
            ::close(s); return 0;
        }
        socklen_t alen = sizeof addr;
        ::getsockname(s, reinterpret_cast<sockaddr *>(&addr), &alen);
        const int port = ntohs(addr.sin_port);
        ::close(s);
        return port;
    }
};

// WS client helper
// Just enough WebSocket to drive a handshake and parse incoming frames.

class WSTestClient {
public:
    explicit WSTestClient(int fd) : fd_(fd) {}
    ~WSTestClient() { if (fd_ >= 0) ::close(fd_); }

    WSTestClient(const WSTestClient&) = delete;
    WSTestClient& operator=(const WSTestClient&) = delete;
    WSTestClient(WSTestClient&& o) noexcept
        : fd_(o.fd_), inbox_(std::move(o.inbox_)) { o.fd_ = -1; }

    // Send the HTTP upgrade and verify the 101 response. Returns true on
    // success, in which case the connection is in WS data mode.
    bool handshake() {
        // Generate a random nonce per client so the server's ACCEPT hash
        // can't accidentally collide and mask a bug.
        std::uint8_t nonce[16];
        std::random_device rd;
        for (auto &b : nonce) b = static_cast<std::uint8_t>(rd());
        const std::string key = base64_encode(nonce, sizeof nonce);

        char req[512];
        std::snprintf(req, sizeof req,
            "GET / HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: %s\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n", key.c_str());
        if (::send(fd_, req, std::strlen(req), 0) <= 0) return false;

        // Read the response - should end with \r\n\r\n.
        std::string resp;
        const auto deadline = std::chrono::steady_clock::now() + 1s;
        while (resp.find("\r\n\r\n") == std::string::npos) {
            if (std::chrono::steady_clock::now() > deadline) return false;
            char buf[1024];
            pollfd pfd{fd_, POLLIN, 0};
            if (::poll(&pfd, 1, 100) <= 0) continue;
            ssize_t n = ::recv(fd_, buf, sizeof buf, 0);
            if (n <= 0) return false;
            resp.append(buf, static_cast<std::size_t>(n));
        }
        return resp.find("101 Switching Protocols") != std::string::npos;
    }

    // Read and parse frames until at least `min_frames` have been received
    // or the timeout expires. Returns the number of frames seen.
    int read_frames(int min_frames, std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        int frames = 0;
        while (frames < min_frames &&
               std::chrono::steady_clock::now() < deadline)
        {
            // Try to parse what we already have.
            for (;;) {
                std::uint8_t op;
                std::string payload;
                int consumed = parse_ws_frame(
                    reinterpret_cast<const std::uint8_t *>(inbox_.data()),
                    inbox_.size(), op, payload);
                if (consumed <= 0) break;
                inbox_.erase(0, static_cast<std::size_t>(consumed));
                if (op == 0x2) frames++;        // binary frame from server
                if (frames >= min_frames) return frames;
            }
            // Otherwise wait for more bytes.
            char buf[4096];
            pollfd pfd{fd_, POLLIN, 0};
            const auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (rem <= 0) break;
            int prc = ::poll(&pfd, 1, static_cast<int>(rem));
            if (prc <= 0) continue;
            ssize_t n = ::recv(fd_, buf, sizeof buf, 0);
            if (n <= 0) break;
            inbox_.append(buf, static_cast<std::size_t>(n));
        }
        return frames;
    }

    void close_now() {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

    int fd() const { return fd_; }

private:
    int         fd_;
    std::string inbox_;
};

// Test fixture
class WsMultiClient : public ::testing::Test {
protected:
    BackendProcess backend;
    void SetUp() override {
        ASSERT_TRUE(backend.start())
            << "Backend failed to start. If the simple-WS binary isn't "
               "built yet:\n"
               "  cmake -B build/backend-simple -S src/backend "
               "-DFORCE_SIMPLE_WS=ON && \\\n"
               "  cmake --build build/backend-simple --config Release";
    }
};

// Tests
TEST_F(WsMultiClient, SingleClientReceivesFrames) {
    int fd = BackendProcess::dial(backend.port());
    ASSERT_GE(fd, 0);
    WSTestClient c(fd);
    ASSERT_TRUE(c.handshake());

    EXPECT_GE(c.read_frames(/*min_frames=*/3, 2s), 3)
        << "Expected at least 3 binary frames within 2s";
}

TEST_F(WsMultiClient, ThreeConcurrentClientsAllReceiveFrames) {
    // The bug we're regression-testing: the old single-tracked accept loop
    // would leave clients 2 and 3 waiting until client 1 disconnected.
    std::vector<WSTestClient> clients;
    clients.reserve(3);
    for (int i = 0; i < 3; i++) {
        int fd = BackendProcess::dial(backend.port());
        ASSERT_GE(fd, 0) << "Client " << i << " failed to connect";
        clients.emplace_back(fd);
        ASSERT_TRUE(clients.back().handshake())
            << "Client " << i << " failed handshake";
    }

    for (std::size_t i = 0; i < clients.size(); i++) {
        EXPECT_GE(clients[i].read_frames(/*min_frames=*/3, 2s), 3)
            << "Client " << i << " did not receive 3 frames within 2s";
    }
}

TEST_F(WsMultiClient, DroppingOneClientDoesNotStallOthers) {
    int fd_a = BackendProcess::dial(backend.port());
    int fd_b = BackendProcess::dial(backend.port());
    ASSERT_GE(fd_a, 0);
    ASSERT_GE(fd_b, 0);
    WSTestClient a(fd_a), b(fd_b);
    ASSERT_TRUE(a.handshake());
    ASSERT_TRUE(b.handshake());

    // Both should see initial frames.
    EXPECT_GE(a.read_frames(2, 2s), 2);
    EXPECT_GE(b.read_frames(2, 2s), 2);

    // Drop A; B should keep receiving without disruption.
    a.close_now();
    EXPECT_GE(b.read_frames(3, 2s), 3)
        << "B stalled after A disconnected - broadcast loop is not "
           "isolating client errors correctly";
}

TEST_F(WsMultiClient, ReconnectAfterDisconnectWorks) {
    {
        int fd = BackendProcess::dial(backend.port());
        ASSERT_GE(fd, 0);
        WSTestClient c(fd);
        ASSERT_TRUE(c.handshake());
        EXPECT_GE(c.read_frames(2, 2s), 2);
    } // c falls out of scope -> close

    // Give the server a beat to notice the close (POLLHUP next iteration).
    std::this_thread::sleep_for(50ms);

    int fd2 = BackendProcess::dial(backend.port());
    ASSERT_GE(fd2, 0);
    WSTestClient c2(fd2);
    ASSERT_TRUE(c2.handshake());
    EXPECT_GE(c2.read_frames(3, 2s), 3);
}

} // namespace
