# Testing

```bash
npm test               # everything: backend (gtest) + renderer (vitest)
npm run test:backend   # C++ tests via ctest
npm run test:renderer  # JS tests via vitest
```

`npm run test:backend` builds the simple-WS variant of the backend (`-DFORCE_SIMPLE_WS=ON`), pulls GoogleTest via `FetchContent`, compiles the test target, and runs the suite via `ctest`. Subsequent runs are <2 s end-to-end (GoogleTest is fetched once and cached). 142 unit + integration tests currently land green across the following test files. See [backend CLI](backend.md) for what those binaries exercise at runtime.

## Backend tests (`tests/backend/`)

### Wire / protocol / WebSocket

- **`test_ws_framing.cpp`** — `parse_ws_frame`, `base64_encode`, `build_ws_binary_header` against RFC 4648 / RFC 6455 vectors, masked/unmasked, 7/16/64-bit length, partial reads, back-to-back frames.
- **`test_ws_multi_client.cpp`** — fork+exec the actual `signalscope-backend` on an ephemeral port and drive it from raw client sockets: single-client baseline, three concurrent clients, one client dropping without stalling the others, reconnect after disconnect.
- **`test_wire_protocol.cpp`** — `serialize_frame` round-trip across every `FrameType` so the renderer parser and the backend serializer can never silently drift.

### DSP pipeline

- **`test_peak_detect.cpp`** — gate / shape / prominence / NMS / `max_peaks` for `DSPPipeline::detect_peaks` against synthetic spectra.
- **`test_hilbert.cpp`** — analytic-signal correctness: sign of the imaginary part, constant magnitude under sinusoidal input, +f vs -f bin dominance.
- **`test_downconverter.cpp`** — NCO + windowed-sinc LPF + decimator: pass-through, frequency translation to DC, attenuation outside the post-decimation passband, length contracts, streaming-equals-one-shot for FIR/NCO state.
- **`test_overlap.cpp`** — FFT window-stride overlap: slide-buffer correctness across the supported {1, 2, 4, 8, 16}× factors; the overlapped path produces bit-equivalent output to a non-overlapped reference at the matching hop.
- **`test_window_calibration.cpp`** — coherent-gain + ENBW per window; a unit-amplitude tone reads ≈ 0 dBFS regardless of window choice.
- **`test_multitaper.cpp`** — sine-taper estimator: K=1 dormancy (matches the single-window path), variance reduction ∝ 1/K, tone calibration, resize-time recomputation.
- **`test_weighting.cpp`** — A/C-weight curves vs IEC 61672 reference values, two-sided symmetry, flatten no-op behavior, empty-span safety.
- **`test_spectrum_history.cpp`** — backend-held deep-history ring: capacity overflow drops oldest, fetch returns newest-first from an offset, fft-size change clears the ring.

### Sources / I/O

- **`test_ring_buffer.cpp`** — single-threaded SPSC contract + a real two-thread producer/consumer property test that asserts strict ordering under the drop-oldest overflow policy.
- **`test_iq_listener.cpp`** — header parsing for the TCP SDR protocol, format decoders for U8IQ/I16IQ/F32IQ/F32Real (all explicit LE), and a real socket round-trip on loopback.
- **`test_file_decoders.cpp`** — WAV / AIFF / raw / SigMF datatype parsing against on-disk fixtures.
- **`test_fft_backends.cpp`** — parameterized correctness suite (impulse, DC, single tone, Parseval, c2c) over every FFT backend compiled into the test binary, plus a cross-backend agreement check when more than one is present.
- **`test_fft_wisdom.cpp`** — *(only built with `-DUSE_FFTW=ON`)* warm-up writes wisdom to `$HOME`, and `FFTW_WISDOM_ONLY` plan creation succeeds for warmed sizes.

## Renderer tests (`tests/renderer/`)

`npm run test:renderer` runs the vitest suite:

- **`wire-parse.test.js`** — every `FrameType` parsed by the renderer, header-always-lands semantics, length-change reallocation, defensive short-payload paths.
- **`freq.test.js`** — `tToFreq` / `freqToT` round-trips for both linear and log axes across one-sided + two-sided + IQ-with-center-frequency cases.

## Related docs

- [Backend CLI](backend.md) — what the multi-client / framing tests actually drive
- [Wire protocol](protocol.md) — the format the round-trip tests pin
