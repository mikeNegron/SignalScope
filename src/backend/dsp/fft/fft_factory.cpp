#include "fft_backend.hpp"
#include "pocketfft_backend.hpp"

#ifdef SIGNALSCOPE_HAVE_FFTW
#  include "fftw_backend.hpp"
#endif

#ifdef SIGNALSCOPE_HAVE_KFR
#  include "kfr_backend.hpp"
#endif

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <string>

namespace ss::fft {

namespace {

BackendKind g_active = BackendKind::Pocketfft;

std::string to_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

bool fftw_compiled_in() {
#ifdef SIGNALSCOPE_HAVE_FFTW
    return true;
#else
    return false;
#endif
}

bool kfr_compiled_in() {
#ifdef SIGNALSCOPE_HAVE_KFR
    return true;
#else
    return false;
#endif
}

BackendKind parse_preference(std::string_view pref) {
    const auto lower = to_lower(pref);
    if (lower.empty() || lower == "auto" || lower == "default") return BackendKind::Pocketfft;
    if (lower == "pocketfft" || lower == "pocket") return BackendKind::Pocketfft;
    if (lower == "fftw" || lower == "fftw3" || lower == "fftw3f") return BackendKind::Fftw;
    if (lower == "kfr") return BackendKind::Kfr;
    std::cerr << "[fft] unknown backend preference '" << pref
              << "', falling back to pocketfft\n";
    return BackendKind::Pocketfft;
}

}  // namespace

// Without an explicit preference, pick whichever optional backend the
// developer compiled in. The presence of -DUSE_FFTW / -DUSE_KFR is a
// build-time signal that they want that backend as the active default;
// otherwise pocketfft (the universal fallback) wins.
static BackendKind compiled_in_default() {
#ifdef SIGNALSCOPE_HAVE_KFR
    return BackendKind::Kfr;
#elif defined(SIGNALSCOPE_HAVE_FFTW)
    return BackendKind::Fftw;
#else
    return BackendKind::Pocketfft;
#endif
}

BackendKind initialize(std::string_view preference) {
    std::string pref(preference);
    if (pref.empty()) {
        if (const char* env = std::getenv("SS_FFT_BACKEND"); env && *env) {
            pref = env;
        }
    }

    BackendKind want = pref.empty() ? compiled_in_default()
                                    : parse_preference(pref);

    if (want == BackendKind::Fftw && !fftw_compiled_in()) {
        std::cerr << "[fft] FFTW requested but not compiled in "
                  << "(rebuild with -DUSE_FFTW=ON); using pocketfft\n";
        want = BackendKind::Pocketfft;
    }
    if (want == BackendKind::Kfr && !kfr_compiled_in()) {
        std::cerr << "[fft] KFR requested but not compiled in "
                  << "(rebuild with -DUSE_KFR=ON); using pocketfft\n";
        want = BackendKind::Pocketfft;
    }

    g_active = want;
    return g_active;
}

BackendKind active_kind() noexcept { return g_active; }

std::string_view active_name() noexcept {
    switch (g_active) {
    case BackendKind::Pocketfft: return "pocketfft";
    case BackendKind::Fftw:      return "fftw";
    case BackendKind::Kfr:       return "kfr";
    }
    return "pocketfft";
}

std::unique_ptr<IFftBackend> make_backend() {
    switch (g_active) {
#ifdef SIGNALSCOPE_HAVE_FFTW
    case BackendKind::Fftw: return make_fftw_backend();
#endif
#ifdef SIGNALSCOPE_HAVE_KFR
    case BackendKind::Kfr:  return make_kfr_backend();
#endif
    case BackendKind::Pocketfft:
    default:
        return make_pocketfft_backend();
    }
}

void global_startup() {
    switch (g_active) {
#ifdef SIGNALSCOPE_HAVE_FFTW
    case BackendKind::Fftw: fftw_global_startup(); return;
#endif
    case BackendKind::Pocketfft:
    case BackendKind::Kfr:
    default:
        return;
    }
}

void warmup(std::span<const std::size_t> sizes, bool (*should_cancel)()) {
    switch (g_active) {
#ifdef SIGNALSCOPE_HAVE_FFTW
    case BackendKind::Fftw: fftw_warmup(sizes, should_cancel); return;
#endif
    case BackendKind::Pocketfft:
    case BackendKind::Kfr:
    default:
        (void)sizes;
        (void)should_cancel;
        return;
    }
}

void global_shutdown() {
    switch (g_active) {
#ifdef SIGNALSCOPE_HAVE_FFTW
    case BackendKind::Fftw: fftw_global_shutdown(); return;
#endif
    case BackendKind::Pocketfft:
    case BackendKind::Kfr:
    default:
        return;
    }
}

}  // namespace ss::fft
