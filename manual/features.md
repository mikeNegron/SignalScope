# Features

The full list of what SignalScope can do today. The [README highlights](../README.md#highlights)
pick a curated subset for first-time visitors; this doc covers everything.

## Visualization

- Spectrogram waterfall with multiple color maps (baudline, viridis, inferno, hot, cool, …)
- Spectrum analyzer with backend-detected peaks, harmonic bars at the fundamental, peak-hold trace
- Waveform oscilloscope, phase plot, histogram, IQ constellation
- Resizable, hideable panels — drag dividers, toggle visibility
- HiDPI-aware GPU text rendering (atlas + mipmaps)

## Signal processing (all backend / native)

- FFT sizes 256 → 65536, six window functions, per-window coherent-gain + ENBW calibration so tone amplitude stays consistent across window choices
- **FFT overlap** — 0 / 50 / 75 % (slide-buffer-based, no extra capture); the renderer's time-axis zoom auto-promotes to higher overlap factors (up to 16×) so each visible row is its own FFT
- **Multitaper estimator** — K ∈ {1, 2, 4, 8} orthogonal sine tapers (Riedel-Sidorenko); averages K squared FFTs for ~1/K spectrum variance
- Linear or **logarithmic** frequency axis (decade ticks + minor stops)
- **Peak hold** — separate trace, doesn't contaminate the spectrogram
- **Digital down-converter** — tune offset + integer decimation, displays effective fs and absolute center frequency
- **Hilbert pre-stage** (optional) — kills conjugate-symmetric mirror images when tuning a real source
- **Equalization** — A/C-weight (IEC 61672) applied per-bin on the backend; Flatten auto-ranges the y-axis from `spec_min`/`spec_max` that the backend ships in the stats frame
- File playback: WAV, AIFF, raw float32, raw int8/16/24/32, SigMF (`.sigmf-data` + `.sigmf-meta` sidecar) with center-frequency awareness
- Loop-forever playback with non-intrusive loop counter

## Sources

- Built-in test signal (stationary tone + sweeping chirp + pulse train) for hardware-free development
- Microphone capture via miniaudio (CoreAudio / ALSA + PulseAudio / WASAPI)
- File playback (formats listed above)
- **Network SDR listener** — accepts a TCP publisher on a configurable port; supports U8IQ, I16IQ, F32IQ, F32Real with an optional 16-byte handshake header. Works with `hackrf_transfer | nc localhost PORT`, GNU Radio, or anything else that speaks LE samples.

## Deep history

- Backend keeps a configurable ring (default 4096 rows) of full-precision dB spectrum frames
- Renderer scrubs back in time via plain mouse wheel on a paused spectrogram; backend serves the rows on request
- Storage decoupled from the GPU texture, so scrolling into a quiet region doesn't lose the −90 to −130 dB band

## Cursors / measurement

- f1 / f2 cursors — click to place, shift+click for second cursor
- Δf, center, ratio, Δ bins, Δ level
- Cursor frequencies are **absolute** (Fc + offset for two-sided sources, log-aware for log axis)

## Export (backend-driven, save-dialog driven)

- Spectrum → CSV / RAW float32
- Waveform → WAV float32 / AIFF int16
- Snapshot PNG (canvas grab, renderer-side because the pixels only exist there)
- All multi-byte fields written explicitly little-endian per WAV / AIFF spec

## Settings persistence

- Versioned namespace so future schema breaks ignore stale keys instead of silently mis-decoding

## Related docs

- [Controls](controls.md) — keyboard and mouse reference
- [Architecture overview](architecture.md) — where each feature lives in the data flow
