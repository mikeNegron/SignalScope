// usePersistedState - drop-in useState replacement that mirrors its
// value to localStorage under `signalscope.settings.<key>`. The
// namespace lets us prefix-sweep all settings at once. PERSIST_VERSION
// gates the read path: bumping it makes the next load ignore the old
// shape instead of mis-parsing it.

import { useEffect, useRef, useState } from 'react';

const NS = 'signalscope.settings.';
const PERSIST_VERSION = 1;

// Coerce a loaded value to the type of `defaultValue` so prior schema
// shapes don't bleed through. Without this, a setting whose UI control
// used to store strings (e.g., a <select> whose onChange forgot to
// parseInt) keeps round-tripping as a string, and any backend handler
// that JSON-parses it as a number throws type_error.302. Coercing
// preserves the user's intent across schema drift, where bumping
// PERSIST_VERSION would silently wipe their preference.
function coerceToDefaultType(value, defaultValue) {
  if (typeof value === typeof defaultValue) return value;
  switch (typeof defaultValue) {
    case 'number': {
      const n = Number(value);
      return Number.isFinite(n) ? n : defaultValue;
    }
    case 'boolean': return Boolean(value);
    case 'string':  return String(value);
    default:        return value;  // objects/arrays: leave the schema match to the caller
  }
}

function readPersisted(key, defaultValue) {
  try {
    const raw = window.localStorage.getItem(NS + key);
    if (raw == null) return defaultValue;
    const parsed = JSON.parse(raw);
    if (parsed?.v !== PERSIST_VERSION) return defaultValue;
    return coerceToDefaultType(parsed.d, defaultValue);
  } catch {
    return defaultValue;
  }
}

function writePersisted(key, value) {
  try {
    window.localStorage.setItem(
      NS + key,
      JSON.stringify({ v: PERSIST_VERSION, d: value })
    );
  } catch {
    // QuotaExceededError or storage disabled - drop the write.
  }
}

export function usePersistedState(key, defaultValue) {
  const [value, setValue] = useState(() => readPersisted(key, defaultValue));
  // Coalesce bursty updates (knob drag) into one write per frame.
  const pendingRef = useRef(null);
  useEffect(() => {
    if (pendingRef.current) cancelAnimationFrame(pendingRef.current);
    pendingRef.current = requestAnimationFrame(() => {
      writePersisted(key, value);
      pendingRef.current = null;
    });
    return () => {
      if (pendingRef.current) {
        cancelAnimationFrame(pendingRef.current);
        // Window-close races RAF - flush synchronously so we don't lose
        // the last tick of state.
        writePersisted(key, value);
        pendingRef.current = null;
      }
    };
  }, [key, value]);
  return [value, setValue];
}
