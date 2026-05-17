// Tests the type-coercion + validator behavior of usePersistedState's
// read path (the React hook itself is exercised via the existing
// renderer tests; here we test the pure helpers directly).
//
// usePersistedState exports its internal helpers for testability via
// a tiny re-export at the bottom of the module.

import { describe, it, expect, beforeEach } from 'vitest';
import { readPersisted, writePersisted } from '../../src/renderer/hooks/use-persisted-state.js';

// Mock localStorage so tests are hermetic.
function makeLocalStorage() {
    const store = new Map();
    return {
        getItem: (k) => (store.has(k) ? store.get(k) : null),
        setItem: (k, v) => store.set(k, v),
        removeItem: (k) => store.delete(k),
        clear: () => store.clear(),
        store,
    };
}

let mockLS;

beforeEach(() => {
    mockLS = makeLocalStorage();
    if (!globalThis.window) globalThis.window = { localStorage: mockLS };
    else globalThis.window.localStorage = mockLS;
});

describe('readPersisted with validator', () => {
    it('applies the validator after coercion', () => {
        // Stored as a string "0" from an old build. Number default + a
        // clamp validator (min 64) should produce 64.
        mockLS.setItem('signalscope.settings.historyChunk',
            JSON.stringify({ v: 1, d: '0' }));
        const got = readPersisted('historyChunk', 512,
            { validate: (v) => Math.max(64, v) });
        expect(got).toBe(64);
    });

    it('validator runs on the default when no stored value', () => {
        const got = readPersisted('missing-key', 32,
            { validate: (v) => Math.max(64, v) });
        expect(got).toBe(64);
    });

    it('validator returning a different type is respected', () => {
        mockLS.setItem('signalscope.settings.bad-val',
            JSON.stringify({ v: 1, d: -5 }));
        const got = readPersisted('bad-val', 100,
            { validate: (v) => v < 0 ? null : v });
        expect(got).toBeNull();
    });

    it('no validator option falls back to the existing coerce behavior', () => {
        mockLS.setItem('signalscope.settings.k',
            JSON.stringify({ v: 1, d: '50' }));
        const got = readPersisted('k', 0);  // number default coerces "50" -> 50
        expect(got).toBe(50);
    });
});

describe('writePersisted', () => {
    it('round-trips through readPersisted on the happy path', () => {
        writePersisted('roundtrip', { a: 1, b: 2 });
        const got = readPersisted('roundtrip', {});
        expect(got).toEqual({ a: 1, b: 2 });
    });
});
