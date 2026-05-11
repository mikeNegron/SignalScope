// Renderer-side wire-protocol parser tests. Counterpart to the C++
// test_wire_protocol.cpp: that test verifies serialize_frame produces
// the right bytes; this one verifies parseFrame reads those bytes back
// correctly and lands them in the buf object the GPU loop reads each
// frame.
//
// Together they form a closed loop - any change to the wire format on
// either side breaks one of these tests.

import { describe, expect, it } from 'vitest';
import { parseFrame } from '../../src/renderer/lib/wire-parse.js';

// Frame builders (mirror src/backend/protocol/protocol.hpp)

// Mirrors src/backend/protocol/protocol.hpp PROTOCOL_VERSION + 16-byte
// FrameHeader layout. If the C++ side bumps version we'll see it here
// as a torrent of failing tests, which is the point.
const HEADER_SIZE = 16;
const PROTOCOL_VERSION = 0x01;

function makeFrame(type, flags, fft_size, frame_id, sample_rate, payload, opts = {}) {
  const buf = new ArrayBuffer(HEADER_SIZE + payload.length * 4);
  const view = new DataView(buf);
  view.setUint8(0, type);
  view.setUint8(1, opts.version ?? PROTOCOL_VERSION);
  view.setUint8(2, flags);
  view.setUint8(3, 0);                   // reserved
  view.setUint16(4, fft_size, true);
  view.setUint16(6, 0, true);            // reserved
  view.setUint32(8, frame_id, true);
  view.setFloat32(12, sample_rate, true);
  for (let i = 0; i < payload.length; i++) {
    view.setFloat32(HEADER_SIZE + i * 4, payload[i], true);
  }
  return buf;
}

// Empty buf with the same shape useBackendConnection initializes.
function freshBuf() {
  return {
    spectrum: null, waveform: null, iq: null, phase: null, histogram: null,
    spectrumHold: null,
    _iqRe: null, _iqIm: null,
    frameId: 0, clipping: false, sampleRate: 48000,
    specSeq: 0, wavSeq: 0, holdSeq: 0, statsSeq: 0,
    spectrumTwoSided: false, sourceIsComplex: false,
    loopCount: 0, sourceMode: 0, centerFreq: 0,
    peaks: [], noiseFloor: -120, fundamentalFreq: 0,
  };
}

// Reject malformed inputs

describe('parseFrame - input validation', () => {
  it('rejects non-ArrayBuffer', () => {
    const b = freshBuf();
    expect(parseFrame(b, 'not a buffer')).toBe(false);
    expect(parseFrame(b, null)).toBe(false);
    expect(parseFrame(b, undefined)).toBe(false);
  });
  it('rejects buffers shorter than the header', () => {
    const b = freshBuf();
    expect(parseFrame(b, new ArrayBuffer(HEADER_SIZE - 1))).toBe(false);
    expect(parseFrame(b, new ArrayBuffer(0))).toBe(false);
  });
  it('rejects frames with a version byte that does not match', () => {
    const b = freshBuf();
    // Future protocol - must be silently dropped, not parsed with the
    // current layout (which would land mis-decoded fields in `buf`).
    const frame = makeFrame(0x00, 0, 0, 0, 48000, [1.0], { version: 0x02 });
    expect(parseFrame(b, frame)).toBe(false);
    expect(b.spectrum).toBe(null);
    expect(b.frameId).toBe(0);
  });
  it('ignores unknown frame types without crashing', () => {
    const b = freshBuf();
    const frame = makeFrame(0xFF, 0, 0, 0, 48000, [1, 2, 3]);
    expect(parseFrame(b, frame)).toBe(false);
    // Unchanged buf - except the header fields, which always land.
    expect(b.spectrum).toBe(null);
    expect(b.specSeq).toBe(0);
  });
});

// Header always lands

describe('parseFrame - header', () => {
  it('writes frameId / clipping / sampleRate on every frame', () => {
    const b = freshBuf();
    parseFrame(b, makeFrame(0x01, 0x01, 4096, 12345, 96000, [0.5]));
    expect(b.frameId).toBe(12345);
    expect(b.clipping).toBe(true);
    expect(b.sampleRate).toBe(96000);
  });
  it('clipping flag toggles correctly', () => {
    const b = freshBuf();
    parseFrame(b, makeFrame(0x01, 0x01, 0, 0, 48000, [0]));
    expect(b.clipping).toBe(true);
    parseFrame(b, makeFrame(0x01, 0x00, 0, 0, 48000, [0]));
    expect(b.clipping).toBe(false);
  });
});

// Spectrum (0x00)

describe('parseFrame - Spectrum (0x00)', () => {
  it('copies payload into buf.spectrum and bumps specSeq', () => {
    const b = freshBuf();
    const data = [1.0, 2.0, 3.0, -1.0];
    parseFrame(b, makeFrame(0x00, 0, 0, 0, 48000, data));
    expect(Array.from(b.spectrum)).toEqual(data);
    expect(b.specSeq).toBe(1);
  });
  it('reuses the same Float32Array when length matches', () => {
    const b = freshBuf();
    parseFrame(b, makeFrame(0x00, 0, 0, 0, 48000, [1, 2, 3]));
    const ref = b.spectrum;
    parseFrame(b, makeFrame(0x00, 0, 0, 0, 48000, [4, 5, 6]));
    // Same buffer, contents updated - proves the steady-state path
    // doesn't allocate per frame.
    expect(b.spectrum).toBe(ref);
    expect(Array.from(b.spectrum)).toEqual([4, 5, 6]);
  });
  it('reallocates when length changes', () => {
    const b = freshBuf();
    parseFrame(b, makeFrame(0x00, 0, 0, 0, 48000, [1, 2]));
    const ref = b.spectrum;
    parseFrame(b, makeFrame(0x00, 0, 0, 0, 48000, [1, 2, 3]));
    expect(b.spectrum).not.toBe(ref);
    expect(b.spectrum.length).toBe(3);
  });
  it('extracts TwoSided + Complex flag bits', () => {
    const b = freshBuf();
    // TwoSided=0x04, Complex=0x08
    parseFrame(b, makeFrame(0x00, 0x04 | 0x08, 0, 0, 48000, [0]));
    expect(b.spectrumTwoSided).toBe(true);
    expect(b.sourceIsComplex).toBe(true);
  });
  it('does not clobber TwoSided/Complex from non-Spectrum frames', () => {
    const b = freshBuf();
    parseFrame(b, makeFrame(0x00, 0x04, 0, 0, 48000, [0]));
    expect(b.spectrumTwoSided).toBe(true);
    // A waveform frame with flags=0 should NOT reset the spectrum hint.
    parseFrame(b, makeFrame(0x01, 0, 0, 0, 48000, [0]));
    expect(b.spectrumTwoSided).toBe(true);
  });
});

// Waveform (0x01)

describe('parseFrame - Waveform (0x01)', () => {
  it('copies payload into buf.waveform and bumps wavSeq', () => {
    const b = freshBuf();
    parseFrame(b, makeFrame(0x01, 0, 0, 0, 48000, [0.1, 0.2, 0.3]));
    expect(Array.from(b.waveform)).toEqual([
      Math.fround(0.1), Math.fround(0.2), Math.fround(0.3),
    ]);
    expect(b.wavSeq).toBe(1);
  });
});

// IQ (0x02) - planar layout

describe('parseFrame - IQ (0x02)', () => {
  it('splits the planar payload into separate I and Q arrays', () => {
    const b = freshBuf();
    // payload[0..3]=I, payload[4..7]=Q
    const iq = [1, 2, 3, 4,    -1, -2, -3, -4];
    parseFrame(b, makeFrame(0x02, 0, 0, 0, 48000, iq));
    expect(Array.from(b._iqRe)).toEqual([1, 2, 3, 4]);
    expect(Array.from(b._iqIm)).toEqual([-1, -2, -3, -4]);
    expect(b.iq.re).toBe(b._iqRe);
    expect(b.iq.im).toBe(b._iqIm);
  });
  it('reuses I/Q buffers when length is unchanged', () => {
    const b = freshBuf();
    parseFrame(b, makeFrame(0x02, 0, 0, 0, 48000, [1, 2, 3, 4]));
    const re = b._iqRe, im = b._iqIm;
    parseFrame(b, makeFrame(0x02, 0, 0, 0, 48000, [5, 6, 7, 8]));
    expect(b._iqRe).toBe(re);
    expect(b._iqIm).toBe(im);
  });
});

// Phase / Histogram

describe('parseFrame - Phase (0x03)', () => {
  it('copies payload into buf.phase', () => {
    const b = freshBuf();
    parseFrame(b, makeFrame(0x03, 0, 0, 0, 48000, [0, Math.PI / 2, Math.PI]));
    expect(Array.from(b.phase)).toEqual([
      0, Math.fround(Math.PI / 2), Math.fround(Math.PI),
    ]);
  });
});

describe('parseFrame - Histogram (0x04)', () => {
  it('copies payload into buf.histogram', () => {
    const b = freshBuf();
    parseFrame(b, makeFrame(0x04, 0, 0, 0, 48000, [0, 0.5, 1, 0.5, 0]));
    expect(Array.from(b.histogram)).toEqual([0, 0.5, 1, 0.5, 0]);
  });
});

// SourceStatus (0x06)

describe('parseFrame - SourceStatus (0x06)', () => {
  it('reads loopCount / sourceMode / sourceIsComplex / centerFreq', () => {
    const b = freshBuf();
    parseFrame(b, makeFrame(0x06, 0, 0, 0, 48000,
      [/*loop*/ 7, /*mode*/ 2, /*complex*/ 1, /*cf*/ 2.4e6]));
    expect(b.loopCount).toBe(7);
    expect(b.sourceMode).toBe(2);
    expect(b.sourceIsComplex).toBe(true);
    expect(b.centerFreq).toBe(2.4e6);
  });
  it('treats payload[2] < 0.5 as not-complex', () => {
    const b = freshBuf();
    parseFrame(b, makeFrame(0x06, 0, 0, 0, 48000, [0, 0, 0, 0]));
    expect(b.sourceIsComplex).toBe(false);
  });
  it('ignores short payloads (defensive)', () => {
    const b = freshBuf();
    b.loopCount = 99;
    parseFrame(b, makeFrame(0x06, 0, 0, 0, 48000, [1, 2])); // only 2 floats
    expect(b.loopCount).toBe(99);
  });
});

// SpectrumHold (0x07)

describe('parseFrame - SpectrumHold (0x07)', () => {
  it('copies into buf.spectrumHold and bumps holdSeq', () => {
    const b = freshBuf();
    parseFrame(b, makeFrame(0x07, 0, 0, 0, 48000, [10, 20, 30]));
    expect(Array.from(b.spectrumHold)).toEqual([10, 20, 30]);
    expect(b.holdSeq).toBe(1);
  });
});

// SpectrumStats (0x08)

describe('parseFrame - SpectrumStats (0x08)', () => {
  it('reads noiseFloor / fundamental / peaks list', () => {
    const b = freshBuf();
    // First land a Spectrum frame so b.spectrum exists (peaks need N).
    parseFrame(b, makeFrame(0x00, 0, 0, 0, 48000,
      new Array(2048).fill(-120)));

    // Stats payload: [noise, fund, K=2, bin0,val0,prom0, bin1,val1,prom1]
    parseFrame(b, makeFrame(0x08, 0, 0, 0, 48000,
      [-95.5, 440, 2,
       100, -10, 80,
       300, -20, 70]));
    expect(b.noiseFloor).toBe(-95.5);
    expect(b.fundamentalFreq).toBe(440);
    expect(b.peaks.length).toBe(2);
    expect(b.peaks[0]).toEqual({ binIdx: 100, value: -10, prom: 80, N: 2048 });
    expect(b.peaks[1]).toEqual({ binIdx: 300, value: -20, prom: 70, N: 2048 });
    expect(b.statsSeq).toBe(1);
  });
  it('clears peaks when K=0', () => {
    const b = freshBuf();
    b.peaks = [{ binIdx: 1, value: 0, prom: 0, N: 0 }];
    parseFrame(b, makeFrame(0x08, 0, 0, 0, 48000, [-100, 0, 0]));
    expect(b.peaks.length).toBe(0);
  });
  it('ignores frames with payload < 3 floats', () => {
    const b = freshBuf();
    b.noiseFloor = -77;
    parseFrame(b, makeFrame(0x08, 0, 0, 0, 48000, [-50, 100])); // too short
    expect(b.noiseFloor).toBe(-77); // unchanged
  });
  it('uses spectrumHold length for N if hold is active', () => {
    const b = freshBuf();
    parseFrame(b, makeFrame(0x07, 0, 0, 0, 48000, new Array(1024).fill(-120)));
    parseFrame(b, makeFrame(0x08, 0, 0, 0, 48000, [-95, 0, 1, 50, -10, 80]));
    expect(b.peaks[0].N).toBe(1024);
  });
});
