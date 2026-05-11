cmake_minimum_required(VERSION 3.20)
project(signalscope-tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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

# Unit tests
# Pure-function coverage for ws_framing.hpp. No sockets, no threads.
signalscope_add_test(test_ws_framing test_ws_framing.cpp)

# Signal-processing correctness for the Hilbert transformer. Catches the
# anti-symmetric-kernel-iteration-direction bug where the analytic signal
# comes out conjugated.
signalscope_add_test(test_hilbert test_hilbert.cpp)

# DSPPipeline::detect_peaks - gate / shape / prominence / NMS / max_peaks.
# Largest single algorithm in the codebase; fully synthetic spectra so the
# tests are fast and deterministic. DSPPipeline's constructor allocates
# FFTW plans, so the test target has to link FFTW too.
signalscope_add_test(test_peak_detect test_peak_detect.cpp)
find_library(FFTW3F_PEAK_LIB NAMES fftw3f REQUIRED)
find_library(FFTW3F_THREADS_PEAK_LIB NAMES fftw3f_threads REQUIRED)
target_link_libraries(test_peak_detect PRIVATE
    ${FFTW3F_PEAK_LIB} ${FFTW3F_THREADS_PEAK_LIB})

# Wire-protocol round-trip - serialize_frame builds a frame; the test
# decodes it with the same little-endian primitives the renderer uses.
# Catches any future drift in header layout, field width, or endian
# assumption between backend and renderer.
signalscope_add_test(test_wire_protocol test_wire_protocol.cpp)

signalscope_add_test(test_file_decoders test_file_decoders.cpp)

# DownConverter (NCO + windowed-sinc FIR + decimator). Tests the three
# stages individually and their interaction across block boundaries.
signalscope_add_test(test_downconverter test_downconverter.cpp)

# SPSCRingBuffer - single-threaded contract tests + a concurrent
# producer/consumer property test that asserts ordering invariants
# under a real two-thread workload.
signalscope_add_test(test_ring_buffer test_ring_buffer.cpp)

# Equalization curves (A/C-weight, flatten). Pure-function, no FFTW.
signalscope_add_test(test_weighting test_weighting.cpp)

# DSPPipeline overlap (FFT window stride / slide buffer). Needs FFTW
# because DSPPipeline's constructor allocates plans.
signalscope_add_test(test_overlap test_overlap.cpp)
find_library(FFTW3F_OVERLAP_LIB         NAMES fftw3f         REQUIRED)
find_library(FFTW3F_THREADS_OVERLAP_LIB NAMES fftw3f_threads REQUIRED)
target_link_libraries(test_overlap PRIVATE
    ${FFTW3F_OVERLAP_LIB} ${FFTW3F_THREADS_OVERLAP_LIB})

# Per-window coherent-gain + ENBW calibration. Tones must read at the
# same dB across window choices - same dependency story as test_overlap.
signalscope_add_test(test_window_calibration test_window_calibration.cpp)
find_library(FFTW3F_WINCAL_LIB         NAMES fftw3f         REQUIRED)
find_library(FFTW3F_THREADS_WINCAL_LIB NAMES fftw3f_threads REQUIRED)
target_link_libraries(test_window_calibration PRIVATE
    ${FFTW3F_WINCAL_LIB} ${FFTW3F_THREADS_WINCAL_LIB})

# Multitaper estimator (sine tapers). Tests K=1 dormancy, variance
# reduction, tone calibration, resize-time recomputation.
signalscope_add_test(test_multitaper test_multitaper.cpp)
find_library(FFTW3F_MT_LIB         NAMES fftw3f         REQUIRED)
find_library(FFTW3F_THREADS_MT_LIB NAMES fftw3f_threads REQUIRED)
target_link_libraries(test_multitaper PRIVATE
    ${FFTW3F_MT_LIB} ${FFTW3F_THREADS_MT_LIB})

# SpectrumHistory ring (backend-held deep history). Pure data
# structure, no FFTW needed.
signalscope_add_test(test_spectrum_history test_spectrum_history.cpp)

# Live SDR listener - header parsing + format decoders + a real socket
# round-trip. Runs entirely on loopback so it's hermetic.
signalscope_add_test(test_iq_listener test_iq_listener.cpp)

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

# FFTW wisdom warm-up. Imports the file the backend wrote and verifies
# WISDOM_ONLY plan creation succeeds for a warmed size. Needs FFTW +
# the threads helper for the same reason main.cpp does.
signalscope_add_test(test_fft_wisdom test_fft_wisdom.cpp)
find_library(FFTW3F_TEST_LIB NAMES fftw3f REQUIRED)
find_library(FFTW3F_THREADS_TEST_LIB NAMES fftw3f_threads REQUIRED)
target_link_libraries(test_fft_wisdom PRIVATE
    ${FFTW3F_TEST_LIB} ${FFTW3F_THREADS_TEST_LIB})
target_compile_definitions(test_fft_wisdom PRIVATE
    SIGNALSCOPE_BACKEND_BIN="${SIGNALSCOPE_BACKEND_BIN}")
