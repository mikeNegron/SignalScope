// Wire-protocol parser - counterpart of the C++ serialize_frame.
// Mutates `buf` in place; the WS lifecycle stays in index.jsx.
//
// Header layout (mirrors src/backend/protocol/protocol.hpp):
//   0:      type             1:      version (must == PROTOCOL_VERSION)
//   2:      flags            3:      reserved (0)
//   4..5:   fft_size (u16)   6..7:   reserved (0)
//   8..11:  frame_id (u32)   12..15: sample_rate (f32)
//   16..:   payload (Float32Array, 4-byte aligned)
//
// All multi-byte fields little-endian. Version mismatch -> drop the
// frame; a future-protocol backend mustn't poison our buf.

export const PROTOCOL_VERSION = 0x01;
export const HEADER_SIZE = 16;

// Returns true if the frame was consumed, false otherwise.
export function parseFrame(buf, arrayBuffer) {
  if (!(arrayBuffer instanceof ArrayBuffer) || arrayBuffer.byteLength < HEADER_SIZE) {
    return false;
  }

  const view = new DataView(arrayBuffer);
  const type        = view.getUint8(0);
  const version     = view.getUint8(1);
  if (version !== PROTOCOL_VERSION) return false;
  const flags       = view.getUint8(2);
  const frameId     = view.getUint32(8, true);
  const sampleRate  = view.getFloat32(12, true);
  const payload     = new Float32Array(arrayBuffer, HEADER_SIZE);

  // Header fields land on every frame regardless of type.
  buf.frameId    = frameId;
  buf.clipping   = (flags & 0x01) !== 0;
  buf.sampleRate = sampleRate;

  // TwoSided + Complex bits only meaningful on Spectrum. Other frames
  // leave them alone so SourceStatus can't clobber them mid-cycle.
  if (type === 0x00) {
    buf.spectrumTwoSided = (flags & 0x04) !== 0;
    buf.sourceIsComplex  = (flags & 0x08) !== 0;
  }

  switch (type) {
    case 0x00: { // Spectrum
      const len = payload.length;
      if (!buf.spectrum || buf.spectrum.length !== len) {
        buf.spectrum = new Float32Array(len);
      }
      buf.spectrum.set(payload);
      buf.specSeq = (buf.specSeq | 0) + 1;
      return true;
    }
    case 0x01: { // Waveform
      const len = payload.length;
      if (!buf.waveform || buf.waveform.length !== len) {
        buf.waveform = new Float32Array(len);
      }
      buf.waveform.set(payload);
      buf.wavSeq = (buf.wavSeq | 0) + 1;
      return true;
    }
    case 0x02: { // IQ - planar (first half I, second half Q)
      const len = payload.length / 2;
      if (!buf._iqRe || buf._iqRe.length !== len) {
        buf._iqRe = new Float32Array(len);
        buf._iqIm = new Float32Array(len);
        buf.iq = { re: buf._iqRe, im: buf._iqIm };
      }
      buf._iqRe.set(payload.subarray(0, len));
      buf._iqIm.set(payload.subarray(len, len * 2));
      return true;
    }
    case 0x03: { // Phase
      const len = payload.length;
      if (!buf.phase || buf.phase.length !== len) {
        buf.phase = new Float32Array(len);
      }
      buf.phase.set(payload);
      return true;
    }
    case 0x04: { // Histogram
      const len = payload.length;
      if (!buf.histogram || buf.histogram.length !== len) {
        buf.histogram = new Float32Array(len);
      }
      buf.histogram.set(payload);
      return true;
    }
    case 0x06: { // SourceStatus: [loop_count, source_mode, is_complex, center_freq_hz]
      if (payload.length >= 4) {
        buf.loopCount       = payload[0] | 0;
        buf.sourceMode      = payload[1] | 0;
        buf.sourceIsComplex = payload[2] >= 0.5;
        buf.centerFreq      = payload[3];
      }
      return true;
    }
    case 0x07: { // SpectrumHold - same length conventions as Spectrum
      const len = payload.length;
      if (!buf.spectrumHold || buf.spectrumHold.length !== len) {
        buf.spectrumHold = new Float32Array(len);
      }
      buf.spectrumHold.set(payload);
      buf.holdSeq = (buf.holdSeq | 0) + 1;
      return true;
    }
    case 0x09: { // HistoryRow - `frame_id` carries the row's offset
                 // from newest (0 = newest). Lazy alloc - most sessions
                 // never scroll into history.
      if (!buf._history) {
        buf._history = { rows: {}, lastSeq: 0 };
      }
      const offset = frameId;
      const row = new Float32Array(payload.length);
      row.set(payload);
      buf._history.rows[offset] = row;
      buf._history.lastSeq = (buf._history.lastSeq | 0) + 1;
      buf.historySeq = buf._history.lastSeq;
      return true;
    }
    case 0x08: { // SpectrumStats - [noise_floor, fundamental_hz, K,
                 //                  K×(bin,value,prom),
                 //                  spec_min_db, spec_max_db]
      if (payload.length < 3) return true;
      buf.noiseFloor      = payload[0];
      buf.fundamentalFreq = payload[1];
      const K = payload[2] | 0;
      // Hold and live share length; either works as the N for peaks.
      const N = (buf.spectrumHold?.length || buf.spectrum?.length || 0);
      if (!Array.isArray(buf.peaks)) buf.peaks = [];
      buf.peaks.length = 0;
      for (let i = 0; i < K; i++) {
        const off = 3 + i * 3;
        if (off + 2 >= payload.length) break;
        buf.peaks.push({
          binIdx: payload[off]     | 0,
          value:  payload[off + 1],
          prom:   payload[off + 2],
          N,
        });
      }
      // Trailing spec_min/spec_max feed the renderer's Flatten EMA
      // (avoids a per-RAF full-spectrum scan). Optional - a backend
      // that omits them just falls through to the default dB range.
      const tailOff = 3 + K * 3;
      if (payload.length >= tailOff + 2) {
        buf.specMin = payload[tailOff];
        buf.specMax = payload[tailOff + 1];
      }
      buf.statsSeq = (buf.statsSeq | 0) + 1;
      return true;
    }
    default:
      return false;
  }
}
