import { useMemo } from "react";
import { safeFreqDenominator } from "../lib/numeric-safe.js";

// Derive sidebar measurements from a snapshot of the backend's SpectrumStats
// frame. Peaks, noise floor and fundamental are computed in C++; the renderer
// just maps bin -> Hz the same way the grid axis does.
//
// `stats` shape (snapshotted at ~4Hz alongside spectrum/waveform):
//   { peaks: [{binIdx,value,prom,N}], noiseFloor, fundamentalFreq }
export function useMeasurements(stats, sampleRate, centerFreq = 0, twoSided = false) {
  return useMemo(() => {
    if (!stats || !stats.peaks?.length) return null;
    if (safeFreqDenominator(sampleRate) == null) return null;
    const peaks = stats.peaks;
    const N = peaks[0].N;
    const binToFreq = (bin) => {
      const t = (bin + 0.5) / N;
      return twoSided ? centerFreq + (t - 0.5) * sampleRate
                      : t * (sampleRate / 2);
    };
    const p0 = peaks[0];
    const p1 = peaks[1];
    const pkF  = binToFreq(p0.binIdx);
    const pk2F = p1 ? binToFreq(p1.binIdx) : 0;
    return {
      pkF, pkV: p0.value,
      pk2F, pk2V: p1 ? p1.value : -999,
      nf:    stats.noiseFloor,
      snr:   p0.value - stats.noiseFloor,
      delta: p1 ? Math.abs(pk2F - pkF) : 0,
      fundamental: stats.fundamentalFreq,
    };
  }, [stats, sampleRate, centerFreq, twoSided]);
}
