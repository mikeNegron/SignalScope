# Building for Distribution

Each `npm run dist:*` command produces a single distributable file per platform.
The default is **pocketfft** (MIT-compatible, header-only, no runtime FFT
dependency); opt-in `:fftw` / `:fftw-static` / `:kfr` variants exist for
local benchmarking but pull GPL into the binary. The FFT backend name is
encoded in every artifact filename so you can tell at a glance which library
you're shipping.

## Default build

```bash
npm run dist:linux     # → release/SignalScope-0.2.0-pocketfft.AppImage
npm run dist:win       # → release/SignalScope-0.2.0-portable-pocketfft.exe
npm run dist:mac       # → release/SignalScope-0.2.0-pocketfft.dmg
```

Each `dist:*` script rebuilds the backend and renderer first, so you never accidentally ship a stale binary.

## Opt-in GPL variants

```bash
npm run dist:linux:fftw          # → SignalScope-0.2.0-fftw.AppImage         (GPL, libfftw3f at runtime)
npm run dist:linux:fftw-static   # → SignalScope-0.2.0-fftw-static.AppImage  (GPL, no runtime FFTW dep)
npm run dist:linux:kfr           # → SignalScope-0.2.0-kfr.AppImage          (GPL, libkfr_dft at runtime)
```

Same `:fftw`, `:fftw-static`, `:kfr` suffixes exist for `dist:win` and `dist:mac`. These artifacts are **not redistributable under MIT** — see the [FFT backends](#fft-backends) section next for the full licensing rationale.

## FFT backends

The DSP pipeline talks to FFT through a small `IFftBackend` interface so the implementation is swappable. Three backends are available:

| Backend     | License        | Default? | CMake flag                  | Notes                                                                                  |
| ----------- | -------------- | -------- | --------------------------- | -------------------------------------------------------------------------------------- |
| pocketfft   | BSD-3-Clause   | ✅       | always compiled             | Header-only (vendored). MIT-compatible. Zero runtime dependency.                       |
| FFTW3       | GPL v2 / paid  | off      | `-DUSE_FFTW=ON`             | Fastest on x86. **Links GPL into the binary** — incompatible with MIT redistribution. |
| KFR         | GPL v2 / paid  | off      | `-DUSE_KFR=ON`              | Modern SIMD library. Same GPL caveat as FFTW.                                          |

**Default at runtime:** the highest-priority backend the build was compiled with — KFR if `-DUSE_KFR=ON` was passed, otherwise FFTW if `-DUSE_FFTW=ON`, otherwise pocketfft. The reasoning: enabling an optional backend at build time is a deliberate signal that you want it active, not a "just compile it in case" toggle.

**Override at runtime:** `--fft-backend=pocketfft|fftw|kfr` or `SS_FFT_BACKEND=…` env var (CLI flag wins). Requesting a backend that isn't compiled in logs a warning and falls back to pocketfft.

**Licensing matters for shipped binaries.** The project source is MIT regardless of CMake flags, but linking against FFTW or KFR pulls GPL into the executable. The `npm run dist:*` scripts deliberately do not enable either, so the artifacts in `release/` stay MIT-clean. Enable the optional backends only for local benchmarking, or accept that any redistributed binary inherits GPL v2.

## CPack tarballs (optional)

For distribution channels that prefer a versioned `.tar.gz` (or `.zip` on Windows) containing the app plus sibling docs, wrap the AppImage with CPack:

```bash
npm run pack:linux             # → release/signalscope-0.2.0-Linux-pocketfft.tar.gz
npm run pack:linux:fftw        # → release/signalscope-0.2.0-Linux-fftw.tar.gz          (GPL)
npm run pack:linux:fftw-static # → release/signalscope-0.2.0-Linux-fftw-static.tar.gz   (GPL)
npm run pack:linux:kfr         # → release/signalscope-0.2.0-Linux-kfr.tar.gz           (GPL)
npm run pack:win               # → release/signalscope-0.2.0-Windows-pocketfft.zip
npm run pack:mac               # → release/signalscope-0.2.0-Darwin-pocketfft.tar.gz
```

Each `pack:*` script chains the corresponding `dist:*` (which builds backend + UI and runs electron-builder), then runs CPack via [packaging/CMakeLists.txt](../packaging/CMakeLists.txt) to bundle the AppImage / DMG / EXE with `README.md` at the archive root. CPack doesn't replace electron-builder — it wraps electron-builder's output for transport.

To add more sibling files to the tarball (sample SigMF captures, a launcher script, release notes, etc.), add `install(FILES …)` rules in [packaging/CMakeLists.txt](../packaging/CMakeLists.txt).

## Runtime dependencies of the distributable

The built artifact bundles Electron + Chromium + the SignalScope code. The default backend uses pocketfft (vendored, header-only), so end users need only:

- **ALSA / PulseAudio** (Linux audio capture) — `libasound2`, `libpulse0`. Present on essentially every desktop Linux.
- **Standard Electron/Chromium libs** — `libnss3`, `libgtk-3-0`, etc. Already part of every desktop install.

If you ship a `:fftw` variant, the binary additionally needs `libfftw3f` at runtime (Debian/Ubuntu: `libfftw3-single3`; Fedora: `fftw-libs-single`; macOS: `brew install fftw`). The `:fftw-static` variant statically links the FFTW archive (`libfftw3f.a` from `libfftw3-dev` / `brew install fftw`) and has no runtime FFT dependency. The `:kfr` variant similarly needs `libkfr_dft` at runtime; there is no static KFR variant in the `dist:*` scripts (build it manually if you need one).

ALSA / PulseAudio remain dynamically linked even in the static build — those are part of the desktop Linux baseline and pinning specific versions hurts portability more than it helps.

## Related docs

- [Backend CLI](backend.md) — running the backend headless after a build
- [Architecture overview](architecture.md) — what the FFT backend plugs into
