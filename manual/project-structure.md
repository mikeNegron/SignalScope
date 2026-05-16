# Project Structure

Annotated file tree. Use this when you're trying to find where a specific
subsystem lives. For *what* each subsystem does, start with
[architecture.md](architecture.md).

```
signalscope/
├── package.json                       # Electron + electron-builder config
├── setup.sh                           # System deps + vendored headers + uWS installer
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
│       ├── CMakeLists.txt             # pocketfft default; -DUSE_FFTW / -DUSE_KFR opt-in
│       ├── main.cpp                   # AppState (+ OutputFrames), threads, command dispatch
│       ├── protocol/
│       │   ├── protocol.hpp           # FrameHeader, FrameType, FrameFlags, serialize_frame
│       │   └── ws_framing.hpp         # WS framing helpers (testable, no I/O)
│       ├── util/
│       │   ├── ring_buffer.hpp        # Lock-free SPSC ring (drop-oldest overflow)
│       │   └── file_exports.hpp       # Backend-side WAV / AIFF / CSV / RAW writers
│       ├── dsp/
│       │   ├── dsp_pipeline.hpp       # Windows, overlap, multitaper, peak detect, noise floor
│       │   ├── downconverter.hpp      # NCO + windowed-sinc FIR + decimator
│       │   ├── hilbert.hpp            # 257-tap Hilbert FIR (image rejection)
│       │   ├── weighting.hpp          # IEC 61672 A/C-weight curves (Flatten = no-op)
│       │   ├── spectrum_history.hpp   # Deep-history ring (mutex-guarded deque)
│       │   └── fft/                   # IFftBackend + pocketfft (default), FFTW, KFR
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
├── assets/                            # App icons (and screenshots once added)
└── release/                           # electron-builder + CPack output
```

## Related docs

- [Architecture overview](architecture.md) — what each subsystem does
- [Testing](testing.md) — which `tests/backend/test_*.cpp` files cover which subsystems
