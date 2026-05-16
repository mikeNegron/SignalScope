# Wire Protocol

SignalScope's renderer and backend talk over WebSocket using a tiny binary frame
format: a 16-byte header followed by a `float32` payload. No JSON for bulk
data — JSON is reserved for the control channel (renderer → backend commands
only). The renderer mirrors this in [`src/renderer/lib/wire-parse.js`](../src/renderer/lib/wire-parse.js);
the backend serializer is in [`src/backend/protocol/protocol.hpp`](../src/backend/protocol/protocol.hpp).
The two have round-trip test coverage in [`tests/backend/test_wire_protocol.cpp`](../tests/backend/test_wire_protocol.cpp)
and [`tests/renderer/wire-parse.test.js`](../tests/renderer/wire-parse.test.js).

## Header layout

16 bytes, all multi-byte fields little-endian:

```
[type:u8][version:u8][flags:u8][reserved:u8]
[fft_size:u16][reserved:u16]
[frame_id:u32][sample_rate:f32]
[float32[] payload]
```

The `version` byte is the wire-protocol version (currently `0x01`). A renderer drops frames whose version doesn't match its own — a backend speaking a future protocol can't silently poison the parser. The two reserved bytes leave room for future header fields and keep the payload at a 4-byte boundary so `Float32Array` can map it directly.

## Frame types

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

## Flags

`Clipping=0x01`, `Overflow=0x02`, `TwoSided=0x04`, `Complex=0x08`. `Complex` reflects the **source** (file IQ / SDR); `TwoSided` reflects the **output spectrum span** (which can be set independently when the DDC or Hilbert pre-stage is active on a real source).

## Commands (renderer → backend)

JSON text frames: `{ "cmd": "set_fft_size", "value": 4096 }` etc. Parsed by nlohmann/json (vendored) and dispatched through a string-keyed table in [`main.cpp`](../src/backend/main.cpp).

## Endianness contract

All multi-byte fields and the float payload are **little-endian** on the wire. Current targets (x86/ARM desktop) are LE-native, and the SDR listener decodes IEEE-754 LE explicitly so a hypothetical BE host stays correct.

## Related docs

- [Architecture overview](architecture.md) — where the protocol sits in the data flow
- [Project structure](project-structure.md) — file paths for the serializer/parser/tests
