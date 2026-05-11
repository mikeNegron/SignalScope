cmake_minimum_required(VERSION 3.20)
project(signalscope-backend LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Options
option(ENABLE_ASAN       "Enable AddressSanitizer"                              OFF)
option(STATIC_FFTW       "Statically link libfftw3f.a (no runtime FFTW dep)"   OFF)
option(FORCE_SIMPLE_WS   "Use the bundled WS server even if uWebSockets exists" OFF)

# Platform detection
if(WIN32)
    add_definitions(-D_WIN32_WINNT=0x0A00 -DNOMINMAX)
endif()

# Sources
set(BACKEND_SOURCES
    main.cpp
)

add_executable(signalscope-backend ${BACKEND_SOURCES})

target_include_directories(signalscope-backend PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

# miniaudio (header-only). Vendored into src/backend/third_party/
# by setup.sh; bump the pinned version there if you need a newer one.
target_compile_definitions(signalscope-backend PRIVATE
    MINIAUDIO_IMPLEMENTATION
)

# nlohmann/json (header-only, bundled)
# Vendored via setup.sh into src/backend/third_party/nlohmann/json.hpp.
# Already discoverable through the third_party include directory above -
# no extra target_include_directories needed. Pinned to v3.12.0 in
# setup.sh; bump there if you need a newer release.

# FFTW3 (single-precision) - required
# Used for both real-input (r2c) and complex-IQ (c2c) FFT paths.
# Two link modes:
#   STATIC_FFTW=OFF (default) - dynamic link against libfftw3f.so / .dylib.
#       Smaller backend binary; AppImage/dist users need libfftw3-single3
#       (or equivalent) installed at runtime.
#   STATIC_FFTW=ON           - link the static archive libfftw3f.a directly.
#       Backend binary grows ~1MB but has zero runtime FFTW dependency,
#       which is what you want for portable AppImage / .dmg distribution.
#       Both .a and .so are provided by `libfftw3-dev` on Debian/Ubuntu and
#       `brew install fftw` on macOS.
if(STATIC_FFTW)
    # Restrict find_library to .a (and .lib on Windows) for this lookup
    # only; restore the global suffix list afterwards so other deps still
    # resolve to their preferred shared libs.
    set(_orig_lib_suffixes ${CMAKE_FIND_LIBRARY_SUFFIXES})
    if(WIN32)
        set(CMAKE_FIND_LIBRARY_SUFFIXES .lib .a)
    else()
        set(CMAKE_FIND_LIBRARY_SUFFIXES .a)
    endif()
    find_library(FFTW3F_STATIC_LIB NAMES fftw3f)
    find_path(FFTW3F_STATIC_INC NAMES fftw3.h)
    set(CMAKE_FIND_LIBRARY_SUFFIXES ${_orig_lib_suffixes})

    if(FFTW3F_STATIC_LIB AND FFTW3F_STATIC_INC)
        target_link_libraries(signalscope-backend PRIVATE ${FFTW3F_STATIC_LIB})
        target_include_directories(signalscope-backend PRIVATE ${FFTW3F_STATIC_INC})
        message(STATUS "FFTW3: static link -> ${FFTW3F_STATIC_LIB}")
    else()
        message(FATAL_ERROR
            "STATIC_FFTW=ON but libfftw3f.a not found.\n"
            "  Debian/Ubuntu: sudo apt install libfftw3-dev\n"
            "  macOS:         brew install fftw\n"
            "Both packages provide the static archive alongside the .so/.dylib.")
    endif()
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(FFTW3F fftw3f)
    endif()

    if(FFTW3F_FOUND)
        target_link_libraries(signalscope-backend PRIVATE ${FFTW3F_LIBRARIES})
        target_include_directories(signalscope-backend PRIVATE ${FFTW3F_INCLUDE_DIRS})
        message(STATUS "FFTW3: dynamic link via pkg-config")
    else()
        find_library(FFTW3F_LIB NAMES fftw3f)
        find_path(FFTW3F_INC NAMES fftw3.h)
        if(FFTW3F_LIB AND FFTW3F_INC)
            target_link_libraries(signalscope-backend PRIVATE ${FFTW3F_LIB})
            target_include_directories(signalscope-backend PRIVATE ${FFTW3F_INC})
            message(STATUS "FFTW3: dynamic link -> ${FFTW3F_LIB}")
        else()
            message(FATAL_ERROR "fftw3f not found. Install: sudo apt install libfftw3-dev")
        endif()
    endif()
endif()

# FFTW3 threads helpers
# fftwf_make_planner_thread_safe() lives in libfftw3f_threads. Required
# so the DSP thread and the wisdom-warmup thread can both create plans
# without corrupting FFTW's internal planner state. Ships alongside
# libfftw3f in libfftw3-dev / brew install fftw.
if(STATIC_FFTW)
    set(_orig_lib_suffixes ${CMAKE_FIND_LIBRARY_SUFFIXES})
    if(WIN32)
        set(CMAKE_FIND_LIBRARY_SUFFIXES .lib .a)
    else()
        set(CMAKE_FIND_LIBRARY_SUFFIXES .a)
    endif()
    find_library(FFTW3F_THREADS_LIB NAMES fftw3f_threads)
    set(CMAKE_FIND_LIBRARY_SUFFIXES ${_orig_lib_suffixes})
else()
    find_library(FFTW3F_THREADS_LIB NAMES fftw3f_threads)
endif()
if(FFTW3F_THREADS_LIB)
    target_link_libraries(signalscope-backend PRIVATE ${FFTW3F_THREADS_LIB})
    message(STATUS "FFTW3 threads: ${FFTW3F_THREADS_LIB}")
else()
    message(FATAL_ERROR
        "fftw3f_threads not found. It ships with libfftw3-dev (Debian/Ubuntu) "
        "and brew's fftw formula. Install the same package that provided "
        "libfftw3f.")
endif()

# uWebSockets (optional - falls back to built-in WS server)
# Not available via apt. Install from source if desired:
#   git clone https://github.com/uNetworking/uSockets.git && cd uSockets && make && cd ..
#   git clone https://github.com/uNetworking/uWebSockets.git
#   sudo cp -r uWebSockets/src /usr/local/include/uwebsockets
#   sudo cp uSockets/uSockets.a /usr/local/lib/libuSockets.a
#   sudo cp uSockets/src/libusockets.h /usr/local/include/
find_path(UWEBSOCKETS_INCLUDE_DIR uwebsockets/App.h
    PATHS
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
        /usr/local/include
        /usr/include
)

find_path(USOCKETS_INCLUDE_DIR libusockets.h
    PATHS
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
        /usr/local/include
        /usr/include
)

find_library(USOCKETS_LIBRARY uSockets
    PATHS /usr/local/lib /usr/lib
)

if(UWEBSOCKETS_INCLUDE_DIR AND USOCKETS_LIBRARY AND NOT FORCE_SIMPLE_WS)
    target_include_directories(signalscope-backend PRIVATE ${UWEBSOCKETS_INCLUDE_DIR})
    target_link_libraries(signalscope-backend PRIVATE ${USOCKETS_LIBRARY})
    message(STATUS "Found uWebSockets: ${UWEBSOCKETS_INCLUDE_DIR}")
else()
    message(STATUS "uWebSockets not found - using bundled simple WebSocket server")
    target_compile_definitions(signalscope-backend PRIVATE USE_SIMPLE_WS=1)
    # Fallback WS server needs OpenSSL for SHA1 handshake
    find_package(OpenSSL QUIET)
    if(OpenSSL_FOUND)
        target_link_libraries(signalscope-backend PRIVATE OpenSSL::Crypto)
    else()
        find_library(CRYPTO_LIB NAMES crypto)
        if(CRYPTO_LIB)
            target_link_libraries(signalscope-backend PRIVATE ${CRYPTO_LIB})
        else()
            message(WARNING "OpenSSL not found - install: sudo apt install libssl-dev")
        endif()
    endif()
endif()

# zlib (required by uWebSockets even when compression is disabled)
find_package(ZLIB REQUIRED)
target_link_libraries(signalscope-backend PRIVATE ZLIB::ZLIB)

# Threading
find_package(Threads REQUIRED)
target_link_libraries(signalscope-backend PRIVATE Threads::Threads)

# Platform-specific audio libs
if(UNIX AND NOT APPLE)
    # Linux: ALSA + PulseAudio for miniaudio
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(ALSA alsa)
        pkg_check_modules(PULSE libpulse)
    endif()

    # Fallback: find directly if pkg-config unavailable
    if(NOT ALSA_FOUND)
        find_library(ALSA_LIB NAMES asound)
        find_path(ALSA_INC NAMES alsa/asoundlib.h)
        if(ALSA_LIB AND ALSA_INC)
            set(ALSA_FOUND TRUE)
            set(ALSA_LIBRARIES ${ALSA_LIB})
            set(ALSA_INCLUDE_DIRS ${ALSA_INC})
        endif()
    endif()
    if(NOT PULSE_FOUND)
        find_library(PULSE_LIB NAMES pulse)
        find_path(PULSE_INC NAMES pulse/pulseaudio.h)
        if(PULSE_LIB AND PULSE_INC)
            set(PULSE_FOUND TRUE)
            set(PULSE_LIBRARIES ${PULSE_LIB})
            set(PULSE_INCLUDE_DIRS ${PULSE_INC})
        endif()
    endif()

    if(ALSA_FOUND)
        target_link_libraries(signalscope-backend PRIVATE ${ALSA_LIBRARIES})
        target_include_directories(signalscope-backend PRIVATE ${ALSA_INCLUDE_DIRS})
        message(STATUS "Found ALSA")
    else()
        message(STATUS "ALSA not found - install: sudo apt install libasound2-dev")
    endif()
    if(PULSE_FOUND)
        target_link_libraries(signalscope-backend PRIVATE ${PULSE_LIBRARIES})
        target_include_directories(signalscope-backend PRIVATE ${PULSE_INCLUDE_DIRS})
        message(STATUS "Found PulseAudio")
    else()
        message(STATUS "PulseAudio not found - install: sudo apt install libpulse-dev")
    endif()
    target_link_libraries(signalscope-backend PRIVATE dl m)
elseif(APPLE)
    find_library(COREAUDIO_LIB CoreAudio)
    find_library(AUDIOUNIT_LIB AudioUnit)
    find_library(COREFOUNDATION_LIB CoreFoundation)
    target_link_libraries(signalscope-backend PRIVATE
        ${COREAUDIO_LIB} ${AUDIOUNIT_LIB} ${COREFOUNDATION_LIB}
    )
elseif(WIN32)
    target_link_libraries(signalscope-backend PRIVATE ole32 winmm)
endif()

# Compiler flags
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(signalscope-backend PRIVATE
        -O3 -march=native -Wall -Wextra -Wpedantic
    )
    if(ENABLE_ASAN)
        target_compile_options(signalscope-backend PRIVATE -fsanitize=address)
        target_link_options(signalscope-backend PRIVATE -fsanitize=address)
    endif()
elseif(MSVC)
    target_compile_options(signalscope-backend PRIVATE /O2 /W4 /arch:AVX2)
endif()

# Install
install(TARGETS signalscope-backend RUNTIME DESTINATION bin)