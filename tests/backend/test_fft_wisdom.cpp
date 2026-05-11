// Verifies the backend's FFTW wisdom warm-up:
//   1. The wisdom file appears at $HOME/.cache/signalscope/fftw-wisdom.
//   2. Its contents are valid FFTW wisdom - importing it lets the test
//      process create a plan with FFTW_WISDOM_ONLY for a warmed size,
//      which would return null without cached wisdom.
//
// Spawns the backend with HOME pointed at a throwaway tempdir so the
// host machine's real wisdom file is never touched.

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <fftw3.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

#ifndef SIGNALSCOPE_BACKEND_BIN
#  error "SIGNALSCOPE_BACKEND_BIN must be defined by the build system"
#endif

using namespace std::chrono_literals;

namespace {

// Make a unique temp directory under /tmp. Caller owns cleanup.
std::filesystem::path make_tmpdir() {
    char tmpl[] = "/tmp/sigscope-wisdom-XXXXXX";
    char *p = ::mkdtemp(tmpl);
    EXPECT_NE(p, nullptr);
    return p ? std::filesystem::path(p) : std::filesystem::path{};
}

// Spawns the backend with HOME=tmphome and a test signal source.
// Returns the child pid (-1 on failure).
pid_t spawn_backend(const std::filesystem::path &tmphome, int port) {
    if (!std::filesystem::exists(SIGNALSCOPE_BACKEND_BIN)) return -1;

    pid_t pid = ::fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        // Child: HOME drives where the wisdom file lands.
        ::setenv("HOME", tmphome.c_str(), 1);
        const std::string port_arg = "--port=" + std::to_string(port);
        ::execl(SIGNALSCOPE_BACKEND_BIN, SIGNALSCOPE_BACKEND_BIN,
                "--test", port_arg.c_str(), static_cast<char *>(nullptr));
        std::_Exit(127);
    }
    return pid;
}

void terminate_and_wait(pid_t pid) {
    if (pid <= 0) return;
    ::kill(pid, SIGTERM);
    int status = 0;
    for (int i = 0; i < 40; i++) {
        if (::waitpid(pid, &status, WNOHANG) == pid) return;
        std::this_thread::sleep_for(25ms);
    }
    ::kill(pid, SIGKILL);
    ::waitpid(pid, &status, 0);
}

// Reserve an ephemeral TCP port (same trick as test_ws_multi_client).
int pick_port() {
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return 0;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int opt = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
    if (::bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof addr) < 0) {
        ::close(s); return 0;
    }
    socklen_t alen = sizeof addr;
    ::getsockname(s, reinterpret_cast<sockaddr *>(&addr), &alen);
    int p = ntohs(addr.sin_port);
    ::close(s);
    return p;
}

} // namespace

TEST(FftWisdom, WarmupCreatesUsableWisdomFile) {
    auto tmphome = make_tmpdir();
    ASSERT_FALSE(tmphome.empty());

    int port = pick_port();
    ASSERT_NE(port, 0);

    pid_t pid = spawn_backend(tmphome, port);
    ASSERT_GT(pid, 0)
        << "Backend binary not found at " << SIGNALSCOPE_BACKEND_BIN
        << "\n  Build it first: npm run build:backend";

    const auto wisdom_path = tmphome / ".cache" / "signalscope" / "fftw-wisdom";

    // Poll up to 15 s for the wisdom file to appear and grow past the
    // empty-file boundary. The smallest size (N=256) finishes in <1 ms,
    // so on a healthy machine this resolves in well under a second.
    // The bigger sizes (32768, 65536) keep the warm-up running for a
    // few seconds longer, but we don't need to wait for them.
    bool found = false;
    for (int i = 0; i < 150 && !found; i++) {
        std::this_thread::sleep_for(100ms);
        std::error_code ec;
        if (std::filesystem::exists(wisdom_path, ec)) {
            const auto sz = std::filesystem::file_size(wisdom_path, ec);
            if (!ec && sz > 100) found = true;
        }
    }

    if (!found) {
        terminate_and_wait(pid);
        std::filesystem::remove_all(tmphome);
        FAIL() << "Wisdom file did not appear at " << wisdom_path
               << " - warm-up thread didn't run, or HOME override didn't take effect.";
    }

    // The backend keeps running while we validate the file contents - fine,
    // we're just reading the export it already wrote. Stop it after the
    // checks below to keep the test deterministic.

    // Validation: import the wisdom into THIS process, then verify that a
    // FFTW_WISDOM_ONLY plan succeeds for a warmed size. Without cached
    // wisdom that flag returns null, so a successful plan proves the file
    // contains real, usable plan data.
    fftwf_make_planner_thread_safe();
    ASSERT_NE(fftwf_import_wisdom_from_filename(wisdom_path.c_str()), 0)
        << "FFTW refused to import the wisdom file - file is malformed.";

    constexpr int N = 256; // first size in the warm-up sequence
    float *in = fftwf_alloc_real(N);
    fftwf_complex *out = fftwf_alloc_complex(N / 2 + 1);
    fftwf_plan p = fftwf_plan_dft_r2c_1d(
        N, in, out, FFTW_MEASURE | FFTW_WISDOM_ONLY);

    EXPECT_NE(p, nullptr)
        << "WISDOM_ONLY plan for N=" << N << " returned null. The wisdom "
           "file exists but doesn't cover the smallest warm-up size, which "
           "means the warm-up thread either didn't run or didn't export "
           "after the first plan completed.";

    if (p) fftwf_destroy_plan(p);
    fftwf_free(in);
    fftwf_free(out);

    // Cleanup
    terminate_and_wait(pid);
    std::error_code ec;
    std::filesystem::remove_all(tmphome, ec);
    // Reset FFTW global state so this test doesn't influence others
    // running in the same process.
    fftwf_forget_wisdom();
}
