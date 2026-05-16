cmake_minimum_required(VERSION 3.20)
project(signalscope-tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Mirror the backend's USE_FFTW / USE_KFR options so the same toggles
# drive what the tests compile against. Default off — pocketfft only.
option(USE_FFTW "Compile FFTW backend into tests"  OFF)
option(USE_KFR  "Compile KFR backend into tests"   OFF)

# GoogleTest / GoogleMock via FetchContent
# Pinned to v1.14.0 - current release at time of writing. Pinning a
# tag (rather than `main`) keeps test runs reproducible across machines.
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
# Don't override the parent project's compiler / runtime selection on Windows.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Test discovery
enable_testing()
include(GoogleTest)

set(BACKEND_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src/backend)

# Common include path so tests can reach the backend headers directly.
function(signalscope_add_test name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE ${BACKEND_SRC})
    target_link_libraries(${name} PRIVATE GTest::gtest GTest::gtest_main GTest::gmock)
    gtest_discover_tests(${name})
endfunction()

# Compile the FFT backend stack into a test target. Mirrors what the
# production CMake adds to signalscope-backend so DSPPipeline-dependent
# tests resolve the same symbols (ss::fft::make_backend, etc.) without
# needing a separate library target.
function(signalscope_link_fft_backend tgt)
    target_sources(${tgt} PRIVATE
        ${BACKEND_SRC}/dsp/fft/fft_factory.cpp
        ${BACKEND_SRC}/dsp/fft/pocketfft_backend.cpp
    )
    target_include_directories(${tgt} PRIVATE ${BACKEND_SRC}/third_party)
    if(USE_FFTW)
        target_sources(${tgt} PRIVATE ${BACKEND_SRC}/dsp/fft/fftw_backend.cpp)
        target_compile_definitions(${tgt} PRIVATE SIGNALSCOPE_HAVE_FFTW=1)
        find_library(FFTW3F_LIB_${tgt}         NAMES fftw3f         REQUIRED)
        find_library(FFTW3F_THREADS_LIB_${tgt} NAMES fftw3f_threads REQUIRED)
        target_link_libraries(${tgt} PRIVATE
            ${FFTW3F_LIB_${tgt}} ${FFTW3F_THREADS_LIB_${tgt}})
    endif()
    if(USE_KFR)
        target_sources(${tgt} PRIVATE ${BACKEND_SRC}/dsp/fft/kfr_backend.cpp)
        target_compile_definitions(${tgt} PRIVATE SIGNALSCOPE_HAVE_KFR=1)
        find_package(KFR CONFIG REQUIRED
            HINTS ${KFR_DIR} ${KFR_ROOT}
            PATHS /usr/local /usr)
        target_link_libraries(${tgt} PRIVATE kfr kfr_dft)
    endif()
endfunction()

# Unit tests
# Pure-function coverage for ws_framing.hpp. No sockets, no threads.
signalscope_add_test(test_ws_framing test_ws_framing.cpp)

# Signal-processing correctness for the Hilbert transformer. Catches the
# anti-symmetric-kernel-iteration-direction bug where the analytic signal
# comes out conjugated.
signalscope_add_test(test_hilbert test_hilbert.cpp)

# DSPPipeline::detect_peaks - gate / shape / prominence / NMS / max_peaks.
# Largest single algorithm in the codebase; fully synthetic spectra so the
# tests are fast and deterministic.
signalscope_add_test(test_peak_detect test_peak_detect.cpp)
signalscope_link_fft_backend(test_peak_detect)

# Wire-protocol round-trip - serialize_frame builds a frame; the test
# decodes it with the same little-endian primitives the renderer uses.
# Catches any future drift in header layout, field width, or endian
# assumption between backend and renderer.
signalscope_add_test(test_wire_protocol test_wire_protocol.cpp)

# Steady-state allocation count. Replaces global operator new with a
# counter; must be its own binary so gtest's infrastructure isn't
# included in the counted window.
signalscope_add_test(test_serialize_alloc_count test_serialize_alloc_count.cpp)

signalscope_add_test(test_file_decoders test_file_decoders.cpp)

# DownConverter (NCO + windowed-sinc FIR + decimator). Tests the three
# stages individually and their interaction across block boundaries.
signalscope_add_test(test_downconverter test_downconverter.cpp)

# SPSCRingBuffer - single-threaded contract tests + a concurrent
# producer/consumer property test that asserts ordering invariants
# under a real two-thread workload.
signalscope_add_test(test_ring_buffer test_ring_buffer.cpp)

# Equalization curves (A/C-weight, flatten). Pure-function, no FFT.
signalscope_add_test(test_weighting test_weighting.cpp)

# Correctness tests for SIMD-vectorized DSP helpers. Compare each
# SIMD entry point against its scalar reference on deterministic input.
signalscope_add_test(test_simd_ops test_simd_ops.cpp)

# DSPPipeline overlap (FFT window stride / slide buffer).
signalscope_add_test(test_overlap test_overlap.cpp)
signalscope_link_fft_backend(test_overlap)

# Per-window coherent-gain + ENBW calibration. Tones must read at the
# same dB across window choices.
signalscope_add_test(test_window_calibration test_window_calibration.cpp)
signalscope_link_fft_backend(test_window_calibration)

# Multitaper estimator (sine tapers). Tests K=1 dormancy, variance
# reduction, tone calibration, resize-time recomputation.
signalscope_add_test(test_multitaper test_multitaper.cpp)
signalscope_link_fft_backend(test_multitaper)

# SpectrumHistory ring (backend-held deep history). Pure data
# structure, no FFT needed.
signalscope_add_test(test_spectrum_history test_spectrum_history.cpp)

# Live SDR listener - header parsing + format decoders + a real socket
# round-trip. Runs entirely on loopback so it's hermetic.
signalscope_add_test(test_iq_listener test_iq_listener.cpp)

# Parameterized correctness suite over every FFT backend compiled into
# the binary (pocketfft always; FFTW / KFR conditionally). No subprocess,
# no sockets — pure in-process unit tests on IFftBackend.
signalscope_add_test(test_fft_backends test_fft_backends.cpp)
signalscope_link_fft_backend(test_fft_backends)

# Microbench binary. Not registered with gtest_discover_tests — it's a
# standalone executable invoked manually (or by scripts/bench_dsp.sh).
add_executable(bench_dsp_pipeline bench_dsp_pipeline.cpp)
target_include_directories(bench_dsp_pipeline PRIVATE ${BACKEND_SRC})
signalscope_link_fft_backend(bench_dsp_pipeline)
# Match the production backend's optimization flags so the bench numbers
# are representative of what ships. Without -march=native the bench
# would run with the baseline x86-64 ISA and produce numbers that
# don't reflect production performance.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(bench_dsp_pipeline PRIVATE -O3 -march=native)
elseif(MSVC)
    target_compile_options(bench_dsp_pipeline PRIVATE /O2 /arch:AVX2)
endif()

# Integration tests
# These spawn a real backend subprocess. The binary location can be
# overridden via -DSIGNALSCOPE_BACKEND_BIN=… for unusual build layouts;
# the default assumes the repo's standard build/ tree.
if(NOT DEFINED SIGNALSCOPE_BACKEND_BIN)
    set(SIGNALSCOPE_BACKEND_BIN
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build/backend-simple/signalscope-backend")
endif()

# Multi-client broadcast path with raw client sockets.
signalscope_add_test(test_ws_multi_client test_ws_multi_client.cpp)
target_compile_definitions(test_ws_multi_client PRIVATE
    SIGNALSCOPE_BACKEND_BIN="${SIGNALSCOPE_BACKEND_BIN}"
)

# FFTW wisdom warm-up. FFTW-specific (the wisdom file format and the
# planner-thread-safe handshake have no equivalent in pocketfft/KFR),
# so this test is only built when USE_FFTW=ON.
if(USE_FFTW)
    signalscope_add_test(test_fft_wisdom test_fft_wisdom.cpp)
    find_library(FFTW3F_TEST_LIB         NAMES fftw3f         REQUIRED)
    find_library(FFTW3F_THREADS_TEST_LIB NAMES fftw3f_threads REQUIRED)
    target_link_libraries(test_fft_wisdom PRIVATE
        ${FFTW3F_TEST_LIB} ${FFTW3F_THREADS_TEST_LIB})
    target_compile_definitions(test_fft_wisdom PRIVATE
        SIGNALSCOPE_BACKEND_BIN="${SIGNALSCOPE_BACKEND_BIN}")
endif()
