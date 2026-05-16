# Running the Backend Standalone

The C++ backend is a normal command-line process and runs perfectly well without
Electron. Use it for headless dashboards, automated tests, remote SDR receivers,
or any case where you want the DSP pipeline without an Electron renderer attached.
It always serves WebSocket on `localhost:8765` unless `--port=N` overrides it.

## Quick reference

After running `npm run build:backend`:

```bash
./build/backend/signalscope-backend --test               # built-in test signal (tone + chirp + pulse train)
./build/backend/signalscope-backend --mic --rate=48000   # microphone capture
./build/backend/signalscope-backend --list-devices       # enumerate capture devices
./build/backend/signalscope-backend --port=9000          # alternate port
./build/backend/signalscope-backend --fft=8192           # FFT size override
```

The renderer always connects to `ws://localhost:8765`. If you launch the Electron app while a backend is already on that port, Electron's own spawn fails to bind (port collision) and the renderer just attaches to the existing one — there's no port-discovery step.

## uWebSockets vs simple-WS

The backend uses [uWebSockets](https://github.com/uNetworking/uWebSockets) when available and falls back to a minimal built-in `poll()`-driven WebSocket server otherwise. **Both paths are multi-client** — the fallback uses a single-threaded event loop that tracks an arbitrary number of concurrent connections. uWebSockets is preferred in production for lower per-connection overhead; install it via `setup.sh`.

To compile-test the fallback path even when uWebSockets is installed, build with `-DFORCE_SIMPLE_WS=ON`. This is also what `npm run test:backend` does internally.

## Related docs

- [Wire protocol](protocol.md) — the binary frame format clients receive
- [Building for distribution](building.md) — packaged variants if you also want the Electron app
- [Testing](testing.md) — how the test suite drives the backend
