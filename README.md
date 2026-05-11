# SignalScope

A browser-class signal analyzer with a native C++ DSP backend — a modern Baudline for the desktop, packaged as a single distributable file.

## Architecture

```
┌──────────────────────────────────────────────────────────────────────────┐
│                            Electron Shell                                │
│                                                                          │
│  ┌──────────────────────────────────────────────────────────────────┐    │
│  │                  C++ Backend (separate process)                  │    │
│  │                                                                  │    │
│  │  Sources         SPSC ring        DSP pipeline                   │    │
│  │  ──────────────  ─────────────    ─────────────────────────────  │    │
│  │  miniaudio mic   1M floats        Hilbert? → DDC? → window       │    │
│  │  file decoders   (~21s @ 48kHz)   → FFTW (overlap, multitaper)   │    │
│  │  test signal     drop-oldest      → A/C-weight? → peak detect    │    │
│  │  TCP IQ listener overflow         → noise floor + fundamental    │    │
│  │                                   → spec_min/max for auto-range  │    │
│  │                                   → SpectrumHistory deep ring    │    │
│  │                                                                  │    │
│  │                                  ↓ binary frames over WebSocket  │    │
│  └──────────────────────────────────┬───────────────────────────────┘    │
│                                     │                                    │
│  ┌──────────────────────────────────▼───────────────────────────────┐    │
│  │                  React + WebGL2 Renderer                         │    │
│  │                                                                  │    │
│  │  ws onmessage (no React) → preallocated bufRef                   │    │
│  │  GPU RAF reads bufRef directly → SpectrumRenderer,               │    │
│  │      SpectrogramRenderer (ring-buffer dB texture, live + history │    │
│  │        modes), WaveformRenderer, PhaseRenderer, IQRenderer,      │    │
│  │      HistogramRenderer, OverlayRenderer (cursors, peaks,         │    │
│  │      harmonic bars), TextRenderer (atlas + mipmaps)              │    │
│  │  React state at 4 Hz only (status bar, sidebar)                  │    │
│  │  Persistent settings in localStorage (signalscope.settings.*)    │    │
│  └──────────────────────────────────────────────────────────────────┘    │
└──────────────────────────────────────────────────────────────────────────┘
```

### Process model

The backend runs as its own native process spawned by Electron's main. The renderer talks to it over `ws://localhost:8765`. Backend and renderer can also be developed independently — start the backend manually and connect any browser at the same URL.

### Three-thread backend pipeline

1. **Capture thread** — miniaudio callback (mic) or `FileSource` worker writes into a lock-free SPSC ring buffer. Never blocks; producer-side allocations are amortized to startup.
2. **DSP thread** — pulls samples, optionally runs the Hilbert pre-stage and DDC (mixer + FIR low-pass + decimator), applies a window (Hanning/Hamming/Blackman/FlatTop/Kaiser/Rectangular), runs FFTW (`r2c` for real input, `c2c` for complex) with configurable overlap and optional K-taper multitaper averaging, produces dBFS magnitude + phase, then runs the spectrum analytics: A/C-weighting (optional), peak picking, noise-floor estimate, fundamental detection, `spec_min`/`spec_max` for the renderer's Flatten auto-range, and a push into the deep-history ring. Optional peak-hold side buffer is updated when enabled.
3. **WebSocket thread** — broadcasts serialized binary frames to every connected renderer at the engine target rate (60 fps default). Drains the deep-history response queue alongside the live frames.

A separate **listener thread** runs when source = `listen`: a TCP server accepts a single SDR publisher, decodes its samples (U8IQ / I16IQ / F32IQ / F32Real, all little-endian on the wire), and writes them straight into the capture ring buffer. See [`source/iq_listener.hpp`](src/backend/source/iq_listener.hpp).

### Renderer model — heavy compute on the backend

The renderer follows the Figma/Discord model: every algorithmic step lives in C++. The browser does **layout, drawing, and user input** — nothing else. Concretely:

- **Peak detection, noise-floor, fundamental** — all in [`dsp_pipeline.hpp`](src/backend/dsp/dsp_pipeline.hpp), sent to the renderer as a `SpectrumStats` frame.
- **Peak hold** — running per-bin max maintained on the backend; sent as a separate `SpectrumHold` frame so the spectrogram waterfall (which always uses the live trace) is unaffected.
- **DDC + Hilbert + A/C-weighting + multitaper averaging + FFT overlap** — backend-only. Renderer just sees the resulting spectrum.
- **Flatten auto-range** — the backend computes `spec_min`/`spec_max` per frame and ships them on the stats frame; the renderer just EMA-smooths and remaps the y-axis.
- **Deep-history scroll** — the spectrogram's ring lives on the backend; the renderer asks for batches of rows via a `request_history` command and the backend replies with `HistoryRow` frames carrying the offset back from newest.
- **Capture exports (WAV / AIFF / CSV / RAW)** — written directly from the backend's serialized frame buffer to a path the user picked via Electron's save dialog. PNG export is the lone renderer-side format because the pixels only exist in the canvas.

The renderer keeps a **stable preallocated buffer** that `onmessage` writes into via `.set()` — no per-frame allocations on the hot path. The GPU RAF loop reads that buffer directly. Status fields (frame ID, clipping, sample rate) are flushed to React state at 4 Hz only, so a sustained 60 fps data stream doesn't force 60 React re-renders per second. Display preferences and DSP knobs round-trip through `localStorage` so they survive reloads; transient state (cursors, pause, view zoom, deep-history scroll position) deliberately does not.

### Binary wire protocol

16-byte header + payload of `float32`s. No JSON for bulk data.

```
[type:u8][version:u8][flags:u8][reserved:u8]
[fft_size:u16][reserved:u16]
[frame_id:u32][sample_rate:f32]
[float32[] payload]
```

The `version` byte is the wire-protocol version (currently `0x01`). A renderer drops frames whose version doesn't match its own — a backend speaking a future protocol can't silently poison the parser. The two reserved bytes leave room for future header fields and keep the payload at a 4-byte boundary so `Float32Array` can map it directly.

| Type | Name           | Payload                                                                                  |
|------|----------------|------------------------------------------------------------------------------------------|
| 0x00 | Spectrum       | `float[N/2]` (real) or `float[N]` (two-sided) — magnitude in dBFS                        |
| 0x01 | Waveform       | `float[N]` — normalized samples [-1, 1]                                                  |
| 0x02 | IQ             | `float[2N]` planar: first N are I, next N are Q                                          |
| 0x03 | Phase          | `float[N/2]` (real) or `float[N]` (two-sided) — radians                                  |
| 0x04 | Histogram      | `float[256]` — amplitude distribution                                                    |
| 0x06 | SourceStatus   | `float[4]` — `[loop_count, source_mode, is_complex, center_freq]`                        |
| 0x07 | SpectrumHold   | `float[N/2 or N]` — peak-hold trace, only sent when hold is on                           |
| 0x08 | SpectrumStats  | `float[3 + 3K + 2]` — `[noise_floor, fund_hz, K, K×(bin,val,prom), spec_min, spec_max]`  |
| 0x09 | HistoryRow     | `float[N/2 or N]` — historical spectrum row; `frame_id` carries the row's offset back    |

(`0x05` was Periodicity — removed when the feature never wired up. Type values are not reused until the next protocol-version bump so older renderers can't mis-decode.)

Flags: `Clipping=0x01`, `Overflow=0x02`, `TwoSided=0x04`, `Complex=0x08`. `Complex` reflects the **source** (file IQ / SDR); `TwoSided` reflects the **output spectrum span** (which can be set independently when the DDC or Hilbert pre-stage is active on a real source).

Commands flow renderer → backend as JSON text frames: `{ "cmd": "set_fft_size", "value": 4096 }` etc. Parsed by nlohmann/json (vendored) and dispatched through a string-keyed table in [`main.cpp`](src/backend/main.cpp). Endianness contract: all multi-byte fields and the float payload are **little-endian** on the wire; current targets (x86/ARM desktop) are LE-native, and the SDR listener decodes IEEE-754 LE explicitly so a hypothetical BE host stays correct.

## Quick Start

```bash
# 1. Clone and enter
git clone <repo> && cd signalscope

# 2. Install system + node dependencies (one-shot)
chmod +x setup.sh && ./setup.sh

# 3. Build the C++ backend
npm run build:backend

# 4. Launch
npm start              # production UI bundle
npm run dev            # development UI bundle (React DevTools)
npm run dev:backend    # dev UI + force Electron to spawn a fresh backend
```

`npm start` runs `prestart` automatically, which rebuilds the renderer bundle. `npm run dev` produces a non-minified bundle that's easier to debug but leaks `PerformanceMeasure` objects under sustained data load — keep it for development only. `npm run dev:backend` adds the `--backend` flag so Electron spawns its own backend process even if one is already listening on `ws://localhost:8765`.

The renderer bundle commands run inside `prestart` / `predist` so you rarely call them directly, but they exist on their own:

```bash
npm run build:ui       # one-shot esbuild → dist/renderer/bundle.{js,css}
npm run dev:ui         # same, but with NODE_ENV=development
```

### Prerequisites

- **Node 22+** and **npm**
- **CMake 3.20+**, a **C++20 compiler** (g++ 13 / clang 16 / MSVC 2022) — backend uses `std::span`, `std::numbers`, `std::bit_ceil`, designated initializers, concepts-adjacent require expressions, etc.
- **fftw3f** (single-precision FFTW) — `libfftw3-dev` on Debian/Ubuntu, `brew install fftw` on macOS
- Linux audio: ALSA (`libasound2-dev`) and PulseAudio (`libpulse-dev`)
- macOS / Windows audio works out of the box (CoreAudio / WASAPI via miniaudio)

`setup.sh` installs the system packages via `apt` if you accept the prompt, downloads the vendored single-header dependencies (nlohmann/json v3.12.0 and miniaudio 0.11.21) into `src/backend/third_party/`, optionally builds + installs uWebSockets from source, and `npm install`s the renderer deps.

## Building for Distribution

Each command produces a single distributable file:

```bash
npm run dist:linux     # → release/SignalScope-0.2.0-shared.AppImage
npm run dist:win       # → release/SignalScope-0.2.0-portable-shared.exe
npm run dist:mac       # → release/SignalScope-0.2.0-shared.dmg
```

The `shared` suffix marks the FFTW link mode (dynamic). The `:static` variants land with `-static` in the filename, so both can coexist in `release/` without clobbering each other. Each `dist:*` script rebuilds the backend and renderer first, so you never accidentally ship a stale binary.

### CPack tarballs (optional)

For distribution channels that prefer a versioned `.tar.gz` (or `.zip` on Windows) containing the app plus sibling docs, wrap the AppImage with CPack:

```bash
npm run pack:linux            # → release/signalscope-0.2.0-Linux-shared.tar.gz
npm run pack:linux:static     # → release/signalscope-0.2.0-Linux-static.tar.gz
npm run pack:win              # → release/signalscope-0.2.0-Windows-shared.zip
npm run pack:win:static       # → release/signalscope-0.2.0-Windows-static.zip
npm run pack:mac              # → release/signalscope-0.2.0-Darwin-shared.tar.gz
npm run pack:mac:static       # → release/signalscope-0.2.0-Darwin-static.tar.gz
```

Each `pack:*` script chains the corresponding `dist:*` (which builds backend + UI and runs electron-builder), then runs CPack via [packaging/CMakeLists.txt](packaging/CMakeLists.txt) to bundle the AppImage / DMG / EXE with `README.md` at the archive root. CPack doesn't replace electron-builder — it wraps electron-builder's output for transport.

To add more sibling files to the tarball (sample SigMF captures, a launcher script, release notes, etc.), add `install(FILES …)` rules in [packaging/CMakeLists.txt](packaging/CMakeLists.txt).

### Runtime dependencies of the distributable

The built artifact bundles Electron + Chromium + the SignalScope code, but the C++ backend dynamically links against a few system libraries. End users need:

- **`libfftw3f` (single-precision FFTW)** — the gotcha. Most distros ship `libfftw3-3` (double-precision) by default but **not** `libfftw3f`. Without it the backend fails to launch with `error while loading shared libraries: libfftw3f.so.3`.
  - Debian/Ubuntu: `sudo apt install libfftw3-single3`
  - Fedora: `sudo dnf install fftw-libs-single`
  - macOS: `brew install fftw`
- **ALSA / PulseAudio** (Linux audio capture) — `libasound2`, `libpulse0`. Present on essentially every desktop Linux.
- **Standard Electron/Chromium libs** — `libnss3`, `libgtk-3-0`, etc. Already part of every desktop install.

If shipping to users who can't or won't install `libfftw3f`, build the **static** variant below.

### Static FFTW build (no runtime FFTW dependency)

The default build dynamically links FFTW (smaller binary, requires `libfftw3f` at runtime). For portable distributables — AppImage / DMG / portable EXE that should run on a fresh machine without extra `apt` calls — link FFTW statically:

```bash
# Standalone backend build with the static archive
npm run build:backend:static

# Or go straight to a self-contained installer:
npm run dist:linux:static     # → release/SignalScope-0.2.0-static.AppImage
npm run dist:win:static       # → release/SignalScope-0.2.0-portable-static.exe
npm run dist:mac:static       # → release/SignalScope-0.2.0-static.dmg

# CPack-wrapped equivalents:
npm run pack:linux:static     # → release/signalscope-0.2.0-Linux-static.tar.gz
npm run pack:win:static       # → release/signalscope-0.2.0-Windows-static.zip
npm run pack:mac:static       # → release/signalscope-0.2.0-Darwin-static.tar.gz
```

Equivalent CMake invocation if you're working outside npm:

```bash
cmake -B build/backend -S src/backend -DSTATIC_FFTW=ON
cmake --build build/backend --config Release
```

The static archive (`libfftw3f.a`) ships alongside the `.so` in `libfftw3-dev` on Debian/Ubuntu and `brew install fftw` on macOS, so no extra setup is needed on the **build** machine. The resulting backend binary is ~1 MB larger but has no runtime FFTW dependency.

ALSA / PulseAudio remain dynamically linked even in the static build — those are part of the desktop Linux baseline and pinning specific versions hurts portability more than it helps.

## C++ Backend Standalone

The backend can run independently for headless or remote use. The renderer connects to whatever URL `BACKEND_WS_URL` points to (default `ws://localhost:8765`).

```bash
# After `npm run build:backend`:
./build/backend/signalscope-backend --test               # built-in test signal (3 tones)
./build/backend/signalscope-backend --mic --rate=48000   # microphone capture
./build/backend/signalscope-backend --list-devices       # enumerate capture devices
./build/backend/signalscope-backend --port=9000          # alternate port
./build/backend/signalscope-backend --fft=8192           # FFT size override
```

Then point any browser at `ws://localhost:8765` (or open the Electron app — it'll see the existing process and connect rather than spawning its own).

### uWebSockets (optional)

The backend uses [uWebSockets](https://github.com/uNetworking/uWebSockets) when available and falls back to a minimal built-in `poll()`-driven WebSocket server otherwise. **Both paths are multi-client** — the fallback uses a single-threaded event loop that tracks an arbitrary number of concurrent connections. uWebSockets is preferred in production for its lower per-connection overhead and battle-tested framing; install it via `setup.sh`.

To compile-test the fallback path even when uWebSockets is installed, build with `-DFORCE_SIMPLE_WS=ON`. This is also what `npm run test:backend` does internally.

## Testing

```bash
npm test               # everything: backend (gtest) + renderer (vitest)
npm run test:backend   # C++ tests via ctest
npm run test:renderer  # JS tests via vitest
```

`npm run test:backend` builds the simple-WS variant of the backend (`-DFORCE_SIMPLE_WS=ON`), pulls GoogleTest via `FetchContent`, compiles the test target, and runs the suite via `ctest`. Subsequent runs are <2 s end-to-end (GoogleTest is fetched once and cached). 142 unit + integration tests currently land green across:

**Wire / protocol / WebSocket**
- **`test_ws_framing.cpp`** — `parse_ws_frame`, `base64_encode`, `build_ws_binary_header` against RFC 4648 / RFC 6455 vectors, masked/unmasked, 7/16/64-bit length, partial reads, back-to-back frames.
- **`test_ws_multi_client.cpp`** — fork+exec the actual `signalscope-backend` on an ephemeral port and drive it from raw client sockets: single-client baseline, three concurrent clients, one client dropping without stalling the others, reconnect after disconnect.
- **`test_wire_protocol.cpp`** — `serialize_frame` round-trip across every `FrameType` so the renderer parser and the backend serializer can never silently drift.

**DSP pipeline**
- **`test_peak_detect.cpp`** — gate / shape / prominence / NMS / `max_peaks` for `DSPPipeline::detect_peaks` against synthetic spectra.
- **`test_hilbert.cpp`** — analytic-signal correctness (sign of the imaginary part, constant magnitude under sinusoidal input, +f vs -f bin dominance) — locks down the convolution-direction bug that produced a conjugated output.
- **`test_downconverter.cpp`** — NCO + windowed-sinc LPF + decimator: pass-through, frequency translation to DC, attenuation outside the post-decimation passband, length contracts, streaming-equals-one-shot for FIR/NCO state.
- **`test_overlap.cpp`** — FFT window-stride overlap: slide-buffer correctness across 50/75/87.5/93.75 % factors; the overlapped path produces bit-equivalent output to a non-overlapped reference at the matching hop.
- **`test_window_calibration.cpp`** — coherent-gain + ENBW per window; a unit-amplitude tone reads ≈ 0 dBFS regardless of window choice.
- **`test_multitaper.cpp`** — sine-taper estimator: K=1 dormancy (matches the single-window path), variance reduction ∝ 1/K, tone calibration, resize-time recomputation.
- **`test_weighting.cpp`** — A/C-weight curves vs IEC 61672 reference values, two-sided symmetry, flatten no-op behavior, empty-span safety.
- **`test_spectrum_history.cpp`** — backend-held deep-history ring: capacity overflow drops oldest, fetch returns newest-first from an offset, fft-size change clears the ring.

**Sources / I/O**
- **`test_ring_buffer.cpp`** — single-threaded SPSC contract + a real two-thread producer/consumer property test that asserts strict ordering under the drop-oldest overflow policy.
- **`test_iq_listener.cpp`** — header parsing for the TCP SDR protocol, format decoders for U8IQ/I16IQ/F32IQ/F32Real (all explicit LE), and a real socket round-trip on loopback.
- **`test_file_decoders.cpp`** — WAV / AIFF / raw / SigMF datatype parsing against on-disk fixtures.
- **`test_fft_wisdom.cpp`** — warm-up writes wisdom to `$HOME`, and `FFTW_WISDOM_ONLY` plan creation succeeds for warmed sizes.

`npm run test:renderer` runs the vitest suite (`tests/renderer/*.test.js`):

- **`wire-parse.test.js`** — every `FrameType` parsed by the renderer, header-always-lands semantics, length-change reallocation, defensive short-payload paths.
- **`freq.test.js`** — `tToFreq` / `freqToT` round-trips for both linear and log axes across one-sided + two-sided + IQ-with-center-frequency cases.

## Project Structure

```
signalscope/
├── package.json                       # Electron + electron-builder config
├── setup.sh                           # System deps + vendored headers + uWS installer
├── DESIGN.md                          # Visual design tokens (dark mode)
├── src/
│   ├── main/
│   │   ├── main.js                    # Electron main; spawns backend, dialog IPC
│   │   └── preload.js                 # contextBridge → window.signalscope
│   ├── renderer/
│   │   ├── index.html                 # Shell HTML; loads bundle.js + bundle.css
│   │   ├── index.jsx                  # WebSocket hook, App, root render
│   │   ├── signal-analyzer.jsx        # Top-level layout + state
│   │   ├── components/
│   │   │   ├── RenderSurface.jsx      # Single canvas + GPU RAF + wheel/click router
│   │   │   ├── sidebar.jsx            # Tab strip + active-tab dispatch
│   │   │   └── sidebar/               # MeasureTab, CursorsTab, PeaksTab, InputTab, SettingsTab
│   │   ├── gpu/                       # WebGL2 renderers (one per panel) + GPURenderer composer
│   │   ├── hooks/
│   │   │   ├── use-persisted-state.js # localStorage-backed useState (namespaced + versioned)
│   │   │   ├── use-panel-layout.js    # Drag dividers + visibility persistence
│   │   │   ├── use-measurements.js    # Stats → sidebar fields
│   │   │   └── use-simulation-loop.js # JS-side fallback when no backend is connected
│   │   ├── lib/
│   │   │   ├── wire-parse.js          # Decode 16-byte header + Float32 payload
│   │   │   ├── freq.js                # Linear/log bin↔Hz mapping, formatting
│   │   │   ├── export.js              # PNG (canvas grab); other formats are backend-driven
│   │   │   ├── generators.js          # Simulation-mode synthetic data
│   │   │   └── tokens.js              # Color + typography tokens
│   │   └── styles/global.css          # Spinner removal, scrollbars, range thumb
│   └── backend/
│       ├── CMakeLists.txt             # Mandatory: fftw3f, zlib, threads, ws server
│       ├── main.cpp                   # AppState (+ OutputFrames), threads, command dispatch
│       ├── protocol/
│       │   ├── protocol.hpp           # FrameHeader, FrameType, FrameFlags, serialize_frame
│       │   └── ws_framing.hpp         # WS framing helpers (testable, no I/O)
│       ├── util/
│       │   ├── ring_buffer.hpp        # Lock-free SPSC ring (drop-oldest overflow)
│       │   └── file_exports.hpp       # Backend-side WAV / AIFF / CSV / RAW writers
│       ├── dsp/
│       │   ├── dsp_pipeline.hpp       # FFT, windows, overlap, multitaper, peak detect, noise floor
│       │   ├── downconverter.hpp      # NCO + windowed-sinc FIR + decimator
│       │   ├── hilbert.hpp            # 257-tap Hilbert FIR (image rejection)
│       │   ├── weighting.hpp          # IEC 61672 A/C-weight curves (Flatten = no-op)
│       │   └── spectrum_history.hpp   # Deep-history ring (mutex-guarded deque)
│       ├── source/
│       │   └── iq_listener.hpp        # TCP SDR publisher listener (LE-explicit decoders)
│       ├── audio/
│       │   ├── audio_capture.hpp      # miniaudio wrapper + test signal
│       │   ├── file_source.hpp        # File-backed worker thread + LoadFileRequest
│       │   └── file_decoders.hpp      # WAV / AIFF / raw / SigMF
│       └── third_party/               # All fetched by setup.sh
│           ├── miniaudio.h            # Audio backend (v0.11.21)
│           └── nlohmann/json.hpp      # JSON parser (v3.12.0)
├── tests/
│   ├── backend/CMakeLists.txt         # gtest/gmock via FetchContent; one binary per test file
│   ├── backend/test_*.cpp             # 15 test binaries (see Testing section)
│   └── renderer/*.test.js             # vitest specs
├── packaging/CMakeLists.txt           # CPack wrapper (TGZ/ZIP of electron-builder output)
├── assets/                            # App icons
└── release/                           # electron-builder + CPack output
```

## Features

**Visualization**
- Spectrogram waterfall with multiple color maps (baudline, viridis, inferno, hot, cool, …)
- Spectrum analyzer with backend-detected peaks, harmonic bars at the fundamental, peak-hold trace
- Waveform oscilloscope, phase plot, histogram, IQ constellation
- Resizable, hideable panels — drag dividers, toggle visibility
- HiDPI-aware GPU text rendering (atlas + mipmaps)

**Signal processing (all backend / native)**
- FFT sizes 256 → 65536, six window functions, per-window coherent-gain + ENBW calibration so tone amplitude stays consistent across window choices
- **FFT overlap** — 0 / 50 / 75 / 87.5 / 93.75 % (slide-buffer-based, no extra capture); the renderer's time-axis zoom auto-promotes to higher overlap factors for Baudline-style "FFT-on-demand"
- **Multitaper estimator** — K ∈ {1, 2, 4, 8} orthogonal sine tapers (Riedel-Sidorenko); averages K squared FFTs for ~1/K spectrum variance
- Linear or **logarithmic** frequency axis (decade ticks + minor stops)
- **Peak hold** — separate trace, doesn't contaminate the spectrogram
- **Digital down-converter** — tune offset + integer decimation, displays effective fs and absolute center frequency
- **Hilbert pre-stage** (optional) — kills conjugate-symmetric mirror images when tuning a real source
- **Equalization** — A/C-weight (IEC 61672) applied per-bin on the backend; Flatten auto-ranges the y-axis from `spec_min`/`spec_max` that the backend ships in the stats frame
- File playback: WAV, AIFF, raw float32, raw int8/16/24/32, SigMF (`.sigmf-data` + `.sigmf-meta` sidecar) with center-frequency awareness
- Loop-forever playback with non-intrusive loop counter

**Sources**
- Built-in test signal (stationary tone + sweeping chirp + pulse train) for hardware-free development
- Microphone capture via miniaudio (CoreAudio / ALSA + PulseAudio / WASAPI)
- File playback (formats listed above)
- **Network SDR listener** — accepts a TCP publisher on a configurable port; supports U8IQ, I16IQ, F32IQ, F32Real with an optional 16-byte handshake header. Works with `hackrf_transfer | nc localhost PORT`, GNU Radio, or anything else that speaks LE samples.

**Deep history (Discord/Figma model)**
- Backend keeps a configurable ring (default 4096 rows) of full-precision dB spectrum frames
- Renderer scrubs back in time via plain mouse wheel on a paused spectrogram; backend serves the rows on request
- Storage decoupled from the GPU texture, so scrolling into a quiet region doesn't lose the −90 to −130 dB band

**Cursors / measurement**
- f1 / f2 cursors — click to place, shift+click for second cursor
- Δf, center, ratio, Δ bins, Δ level
- Cursor frequencies are **absolute** (Fc + offset for two-sided sources, log-aware for log axis)

**Export (backend-driven, save-dialog driven)**
- Spectrum → CSV / RAW float32
- Waveform → WAV float32 / AIFF int16
- Snapshot PNG (canvas grab, renderer-side because the pixels only exist there)
- All multi-byte fields written little-endian explicitly; WAV/AIFF/RAW remain byte-compatible with the previous JS-based exporters

**Settings persistence**
- ~22 UI + DSP fields mirrored to `localStorage` and replayed on reconnect via a `usePersistedState` hook
- Versioned namespace so future schema breaks ignore stale keys instead of silently mis-decoding

## Controls

### Cursor placement (always available)
| Action | Result |
|---|---|
| Click on spectrum or spectrogram | Place f1 cursor (pink) |
| Shift+click | Place f2 cursor (purple) |
| Top-bar `f1` / `f2` buttons | Clear the corresponding cursor |
| Top-bar `CLR` button | Clear both cursors |

f1/f2 are stored in absolute Hz, so they remain anchored to a real frequency even when sample rate, center frequency, frequency scale, or zoom changes.

### Pause / play

| Key | Result |
|---|---|
| **Spacebar** | Toggle pause/play. The DSP loop keeps producing into the deep-history ring while paused, so scrubbing back stays available. |

### Zoom / pan (paused only)

The spectrum and spectrogram share an x-axis viewport. Zoom controls activate when paused; on un-pause the view snaps back to full and any partial drag is cancelled.

| Action | Result |
|---|---|
| **Drag** across the spectrum or spectrogram | Marquee-zoom — release to snap viewport to that x-range. A translucent cyan rectangle tracks the selection. |
| **Alt + wheel** over spectrum or spectrogram | Frequency-axis (X) zoom, anchored at the cursor |
| **Shift + wheel** over the spectrogram | Time-axis (Y) zoom, anchored at the cursor row |
| **Plain wheel** over the spectrogram | Deep-history scroll — advances toward older rows; bounded by `historyCapacity` (default 4096) |
| **Double-click** on spectrum or spectrogram | Reset viewport to the full axis |
| Click without dragging (≤ 4 px movement) | Falls through to cursor placement (same as running mode) |

The zoom is purely a viewport transform — the backend keeps producing full-range data while paused, and the renderer maps a window of that data to the panel. The grid ticks, freq labels, cursor positions, peak markers, and harmonic bars all reposition to the visible window. Peaks that fall outside the window are hidden rather than clamped to the edge.

Deep-history scrolling fires a `request_history` command to the backend, which responds with a batch of `HistoryRow` frames from its in-memory ring. The renderer accumulates them into a separate texture path so live + history don't interfere.

### Sidebar tabs
| Tab | Purpose |
|---|---|
| **Measure** | Connection status, frame rate, source label, clipping indicator |
| **Cursors** | f1/f2 details + Δ analysis (Δf, center, ratio, Δ bins, Δ level) |
| **Peaks** | Detected peaks (color-matched to the circles on the spectrum), noise floor, fundamental, **Min Level** threshold (dBFS) |
| **Input** | Source selection (test / mic / file / **listen**), sample rate, file browse, reconnect. The **Listen** mode accepts an SDR publisher over TCP — see `iq_listener.hpp` for the wire format. |
| **Settings** | Brightness / contrast / scroll, freq scale (linear/log), Down Mixer (Tune / Decimate / Suppress Image), Equalization (none / flatten / A-weight / C-weight), FFT Overlap (0/50/75/87.5/93.75%), Multitaper K (1/2/4/8), History (depth + per-request chunk), Export buttons |

Settings are mirrored to `localStorage` under the `signalscope.settings.*` namespace and replayed on reconnect — display preferences, DSP knobs, and SDR-listener fields survive reloads. Transient state (cursors, pause, view zoom, deep-history scroll position, current source mode) is deliberately not persisted.

## License

MIT
