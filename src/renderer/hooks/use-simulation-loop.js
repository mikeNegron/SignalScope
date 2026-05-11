import { useEffect, useRef } from "react";
import {
  genSpectrum,
  genWaveform,
  genIQ,
  genPhase,
  genHistogram,
} from "../lib/generators.js";

export function useSimulationLoop({ paused, fftSize, backendActive, windowFn = "hanning", gain = 0, simulationEnabled = true }) {
  const timeRef = useRef(0);
  const frameRef = useRef(null);
  const backendActiveRef = useRef(false);
  backendActiveRef.current = backendActive;

  const windowFnRef = useRef(windowFn);
  const gainRef = useRef(gain);
  windowFnRef.current = windowFn;
  gainRef.current = gain;

  // Written by the sim RAF loop, read by the GPU RAF loop - no React state,
  // no re-renders per frame.
  const simDataRef = useRef({
    spectrum: null, waveform: null, iq: null, phase: null, histogram: null,
    specSeq: 0, wavSeq: 0,
    // Mirror the live buffer's analytics fields so the GPU renderer doesn't
    // need a special branch for sim mode. Backend supplies real values.
    peaks: [], noiseFloor: -120, fundamentalFreq: 0,
  });

  useEffect(() => {
    if (paused || !simulationEnabled) {
      if (frameRef.current) {
        cancelAnimationFrame(frameRef.current);
        frameRef.current = null;
      }
      return;
    }
    const animate = () => {
      if (backendActiveRef.current) {
        frameRef.current = null;
        return;
      }
      timeRef.current += 0.05;
      const w = genWaveform(512, timeRef.current);
      simDataRef.current.spectrum  = genSpectrum(+fftSize, timeRef.current, windowFnRef.current, gainRef.current);
      simDataRef.current.specSeq++;
      simDataRef.current.waveform  = w;
      simDataRef.current.wavSeq++;
      simDataRef.current.iq        = genIQ(256, timeRef.current);
      simDataRef.current.phase     = genPhase(+fftSize, timeRef.current);
      simDataRef.current.histogram = genHistogram(w);
      frameRef.current = requestAnimationFrame(animate);
    };
    frameRef.current = requestAnimationFrame(animate);
    return () => {
      if (frameRef.current) {
        cancelAnimationFrame(frameRef.current);
        frameRef.current = null;
      }
    };
  }, [paused, fftSize, simulationEnabled]);

  return { simDataRef, timeRef };
}
