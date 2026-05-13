#!/usr/bin/env bash
# SignalScope — dependency setup
set -euo pipefail

THIRD_PARTY="src/backend/third_party"
BUILD_DIR="build/third_party"
mkdir -p "$THIRD_PARTY" "$BUILD_DIR"

download_file() {
    local url="$1"
    local output="$2"

    if command -v curl &>/dev/null; then
        curl -fsSL "$url" -o "$output"
    elif command -v wget &>/dev/null; then
        wget -qO "$output" "$url"
    else
        echo "  ✗ Neither curl nor wget is available." >&2
        echo "    Install one of them and try again." >&2
        return 1
    fi
}

download_tgz_to_dir() {
    local url="$1"
    local dest="$2"

    local tmp_archive
    tmp_archive="$(mktemp)"

    rm -rf "$dest"
    mkdir -p "$dest"

    download_file "$url" "$tmp_archive"
    tar -xzf "$tmp_archive" --strip-components=1 -C "$dest"

    rm -f "$tmp_archive"
}

echo "╔══════════════════════════════════════════╗"
echo "║       SignalScope Dependency Setup       ║"
echo "╚══════════════════════════════════════════╝"
echo ""

# ─── 1. System packages (apt) ────────────────────────────────────
echo "── [1/4] System packages ──────────────────"
echo ""

SYSTEM_PKGS=(
    # Electron / Chromium runtime dependencies
    libnspr4 libnss3
    libatk1.0-0 libatk-bridge2.0-0
    libcups2 libdrm2 libxkbcommon0
    libxcomposite1 libxdamage1 libxfixes3 libxrandr2
    libgbm1 libpango-1.0-0 libcairo2
    libgtk-3-0 libnotify4 libxss1 libxtst6
    libatspi2.0-0 libsecret-1-0
    xdg-utils

    # C++ backend build toolchain
    build-essential     # gcc/g++ + make
    cmake               # CMakeLists.txt driver
    pkg-config          # used by CMake to find fftw3f / alsa / pulse
    curl wget

    # C++ backend libraries
    zlib1g-dev          # zlib — required (uWebSockets fallback path)
    libssl-dev          # OpenSSL — fallback WebSocket server's SHA-1
    libasound2-dev      # ALSA — Linux audio capture
    libpulse-dev        # PulseAudio — Linux audio capture
)

MISSING=()
for pkg in "${SYSTEM_PKGS[@]}"; do
    if ! dpkg -s "$pkg" &>/dev/null 2>&1; then
        MISSING+=("$pkg")
    fi
done

if [ ${#MISSING[@]} -gt 0 ]; then
    echo "  Missing packages: ${MISSING[*]}"
    echo ""
    read -p "  Install with apt? [Y/n] " -n 1 -r
    echo ""
    if [[ ! $REPLY =~ ^[Nn]$ ]]; then
        sudo apt update -qq
        sudo apt install -y -qq "${MISSING[@]}"
        echo "  ✓ System packages installed"
    else
        echo "  ⚠ Skipped. Install later with:"
        echo "    sudo apt install ${MISSING[*]}"
    fi
else
    echo "  ✓ All system packages present"
fi
echo ""

# ─── 2. Vendored header-only C++ libraries ───────────────────────
echo "── [2/4] Vendored headers ─────────────────"
echo ""
# Both libraries are single-header, pinned to a specific tag for
# reproducible re-runs across machines, and only fetched if missing
# so subsequent runs of setup.sh stay fast.
#
# pocketfft was removed in favor of FFTW3 (installed above via
# libfftw3-dev) and isn't part of this set.

NLOHMANN_VERSION="v3.12.0"
NLOHMANN_DEST="$THIRD_PARTY/nlohmann/json.hpp"
if [ ! -f "$NLOHMANN_DEST" ]; then
    echo "  Downloading nlohmann/json ${NLOHMANN_VERSION}..."
    mkdir -p "$(dirname "$NLOHMANN_DEST")"
    download_file \
        "https://github.com/nlohmann/json/releases/download/${NLOHMANN_VERSION}/json.hpp" \
        "$NLOHMANN_DEST"
    echo "  ✓ nlohmann/json installed to $NLOHMANN_DEST"
else
    echo "  ✓ nlohmann/json already present"
fi

MINIAUDIO_VERSION="0.11.21"
MINIAUDIO_DEST="$THIRD_PARTY/miniaudio.h"
if [ ! -f "$MINIAUDIO_DEST" ]; then
    echo "  Downloading miniaudio ${MINIAUDIO_VERSION}..."
    mkdir -p "$(dirname "$MINIAUDIO_DEST")"
    download_file \
        "https://raw.githubusercontent.com/mackron/miniaudio/${MINIAUDIO_VERSION}/miniaudio.h" \
        "$MINIAUDIO_DEST"
    echo "  ✓ miniaudio installed to $MINIAUDIO_DEST"
else
    echo "  ✓ miniaudio already present"
fi

# pocketfft (single-header, BSD-3-Clause). Default FFT backend.
# MIT-safe — unlike FFTW/KFR (GPL), shipping a pocketfft-linked binary
# under MIT is fine.
POCKETFFT_REVISION="5f27d5a8f51c5c25030cb22abf434decc9faf0ff"
POCKETFFT_DEST="$THIRD_PARTY/pocketfft_hdronly.h"
if [ ! -f "$POCKETFFT_DEST" ]; then
    echo "  Downloading pocketfft (rev ${POCKETFFT_REVISION:0:10})..."
    mkdir -p "$(dirname "$POCKETFFT_DEST")"
    download_file \
        "https://raw.githubusercontent.com/mreineck/pocketfft/${POCKETFFT_REVISION}/pocketfft_hdronly.h" \
        "$POCKETFFT_DEST"
    echo "  ✓ pocketfft installed to $POCKETFFT_DEST"
else
    echo "  ✓ pocketfft already present"
fi
echo ""

# ─── 3. uWebSockets (optional, built from source) ────────────────
echo "── [3/4] uWebSockets (optional) ───────────"
echo ""

UWS_INSTALLED=false
if [ -f /usr/local/include/uwebsockets/App.h ] && [ -f /usr/local/lib/libuSockets.a ]; then
    echo "  ✓ uWebSockets already installed"
    UWS_INSTALLED=true
else
    echo "  uWebSockets provides a high-performance WebSocket server."
    echo "  Without it, a built-in fallback server is used (works fine"
    echo "  for single-client use)."
    echo ""
    read -p "  Build and install uWebSockets from source? [y/N] " -n 1 -r
    echo ""
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        pushd "$BUILD_DIR" > /dev/null

        echo "  Downloading uSockets..."
        if [ ! -d "uSockets" ]; then
            download_tgz_to_dir \
                "https://github.com/uNetworking/uSockets/archive/refs/heads/master.tar.gz" \
                "uSockets"
        fi

        echo "  Building uSockets..."
        cd uSockets
        make -j"$(nproc)"
        sudo cp src/libusockets.h /usr/local/include/
        sudo cp uSockets.a /usr/local/lib/libuSockets.a
        cd ..

        echo "  Downloading uWebSockets..."
        if [ ! -d "uWebSockets" ]; then
            download_tgz_to_dir \
                "https://github.com/uNetworking/uWebSockets/archive/refs/heads/master.tar.gz" \
                "uWebSockets"
        fi

        echo "  Installing uWebSockets headers..."
        sudo mkdir -p /usr/local/include/uwebsockets
        sudo cp uWebSockets/src/*.h /usr/local/include/uwebsockets/

        popd > /dev/null
        echo "  ✓ uWebSockets installed to /usr/local"
        UWS_INSTALLED=true
    else
        echo "  ⚠ Skipped — fallback WebSocket server will be used"
    fi
fi
echo ""

# ─── 4. Node.js dependencies ─────────────────────────────────────
echo "── [4/4] Node.js dependencies ─────────────"
echo ""

if command -v node &>/dev/null; then
    echo "  Node.js: $(node --version)"
    if [ -d "node_modules" ]; then
        echo "  ✓ node_modules present (run 'npm install' to update)"
    else
        echo "  Installing npm packages..."
        npm install
        echo "  ✓ npm packages installed"
    fi
else
    echo "  ⚠ Node.js not found. Install via:"
    echo "    curl -fsSL https://deb.nodesource.com/setup_22.x | sudo -E bash -"
    echo "    # or, if curl is unavailable:"
    echo "    wget -qO- https://deb.nodesource.com/setup_22.x | sudo -E bash -"
    echo "    sudo apt install -y nodejs"
fi
echo ""

# ─── Summary ─────────────────────────────────────────────────────
echo "╔══════════════════════════════════════════╗"
echo "║              Setup Complete              ║"
echo "╚══════════════════════════════════════════╝"
echo ""
echo "  WebSocket server: $([ "$UWS_INSTALLED" = true ] && echo "uWebSockets" || echo "Built-in fallback")"
echo "  FFT engine:       pocketfft (BSD-3, default — MIT-safe binaries)"
echo "                    Optional: -DUSE_FFTW=ON (GPL) -DUSE_KFR=ON (GPL)"
echo "  JSON parser:      nlohmann/json ${NLOHMANN_VERSION} (vendored)"
echo "  Audio backend:    miniaudio ${MINIAUDIO_VERSION} (vendored)"
echo "  FFT backend:      pocketfft (vendored, single-header)"
echo ""
echo "  Next steps:"
echo "    npm run build:backend    # Compile C++ backend"
echo "    npm start                # Launch in dev mode"
echo ""
echo "  Build for distribution:"
echo "    npm run dist:linux       # → AppImage (single file)"
echo "    npm run dist:win         # → Portable .exe"
echo "    npm run dist:mac         # → .dmg"
echo ""