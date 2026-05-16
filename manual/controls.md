# Controls

Keyboard and mouse reference for the SignalScope renderer. f1/f2 cursors are
stored in absolute Hz, so they remain anchored to a real frequency even when
sample rate, center frequency, frequency scale, or zoom changes.

## Cursor placement (always available)

| Action | Result |
|---|---|
| Click on spectrum or spectrogram | Place f1 cursor (pink) |
| Shift+click | Place f2 cursor (purple) |
| Sidebar **Cursors** tab → `Clear f1` / `Clear f2` | Clear the corresponding cursor |
| Sidebar **Cursors** tab → `Clear All` | Clear both cursors |

## Pause / play

| Key | Result |
|---|---|
| **Spacebar** | Toggle pause/play. Suppressed while focus is in an input / textarea / select. |
| Top-bar **PAUSE** / **PLAY** buttons | Same as the spacebar toggle. |

## Zoom / pan (paused only)

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

## Sidebar tabs

| Tab | Purpose |
|---|---|
| **Measure** | Connection status, frame rate, source label, clipping indicator |
| **Cursors** | f1/f2 details + Δ analysis (Δf, center, ratio, Δ bins, Δ level) |
| **Peaks** | Detected peaks (color-matched to the circles on the spectrum), noise floor, fundamental, **Min Level** threshold (dBFS) |
| **Input** | Source selection (test / mic / file / **listen**), sample rate, file browse, reconnect. The **Listen** mode accepts an SDR publisher over TCP — see [`iq_listener.hpp`](../src/backend/source/iq_listener.hpp) for the wire format. |
| **Settings** | Brightness / contrast / scroll, freq scale (linear/log), Down Mixer (Tune / Decimate / Suppress Image), Equalization (none / flatten / A-weight / C-weight), FFT Overlap (0/50/75%), Multitaper K (1/2/4/8), History (depth + per-request chunk), Export buttons |

Settings are mirrored to `localStorage` under the `signalscope.settings.*` namespace and replayed on reconnect — display preferences, DSP knobs, and SDR-listener fields survive reloads. Transient state (cursors, pause, view zoom, deep-history scroll position, current source mode) is deliberately not persisted.

## Related docs

- [Features](features.md) — the capabilities these controls drive
- [Wire protocol](protocol.md) — `request_history` and other commands fired by the controls
