# CPack wrapper around the electron-builder output. Invoked via the
# `npm run pack:*` scripts; assumes the matching `dist:*` has already
# produced the AppImage / DMG / EXE in `release/`.

cmake_minimum_required(VERSION 3.20)

# LANGUAGES NONE - no compiler probe; this project only runs CPack.
project(signalscope-package
    VERSION 0.2.0
    DESCRIPTION "CPack wrapper around the electron-builder distributable"
    LANGUAGES NONE)

set(SIGNALSCOPE_VARIANT "pocketfft" CACHE STRING
    "Which electron-builder artifact to package: pocketfft (default, MIT), "
    "fftw (GPL, dynamic link), fftw-static (GPL, static link), or kfr (GPL)")
set_property(CACHE SIGNALSCOPE_VARIANT PROPERTY STRINGS
    pocketfft fftw fftw-static kfr)
if(NOT SIGNALSCOPE_VARIANT MATCHES "^(pocketfft|fftw|fftw-static|kfr)$")
    message(FATAL_ERROR
        "SIGNALSCOPE_VARIANT must be one of: pocketfft, fftw, fftw-static, kfr. "
        "Got '${SIGNALSCOPE_VARIANT}'.")
endif()

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# Filenames mirror the artifactName templates in package.json.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    set(EB_ARTIFACT
        "${REPO_ROOT}/release/SignalScope-${PROJECT_VERSION}-${SIGNALSCOPE_VARIANT}.AppImage")
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    set(EB_ARTIFACT
        "${REPO_ROOT}/release/SignalScope-${PROJECT_VERSION}-${SIGNALSCOPE_VARIANT}.dmg")
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
    set(EB_ARTIFACT
        "${REPO_ROOT}/release/SignalScope-${PROJECT_VERSION}-portable-${SIGNALSCOPE_VARIANT}.exe")
else()
    message(FATAL_ERROR "Unsupported host system: ${CMAKE_HOST_SYSTEM_NAME}")
endif()

if(NOT EXISTS "${EB_ARTIFACT}")
    message(FATAL_ERROR
        "electron-builder artifact missing:\n  ${EB_ARTIFACT}\n\n"
        "Run: npm run pack:linux:${SIGNALSCOPE_VARIANT}  (chains dist + cpack)")
endif()

install(PROGRAMS "${EB_ARTIFACT}" DESTINATION .)
install(FILES    "${REPO_ROOT}/README.md" DESTINATION .)

set(CPACK_PACKAGE_NAME           "signalscope")
set(CPACK_PACKAGE_VERSION        "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VENDOR         "SignalScope")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Signal analyzer with native C++ DSP backend")
set(CPACK_PACKAGE_FILE_NAME
    "signalscope-${PROJECT_VERSION}-${CMAKE_HOST_SYSTEM_NAME}-${SIGNALSCOPE_VARIANT}")
set(CPACK_PACKAGE_DIRECTORY      "${REPO_ROOT}/release")
# OFF so files extract at the archive root (no `<pkg>-<ver>/` wrapper).
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)

if(WIN32)
    set(CPACK_GENERATOR "ZIP")
else()
    set(CPACK_GENERATOR "TGZ")
endif()

include(CPack)
