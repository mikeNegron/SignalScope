// Renderer <-> backend integration: catches the race-condition class of
// bug where the renderer sends set_* commands at the wrong moment and
// the backend never sees them. Spawns a real backend, connects from
// Node's built-in WebSocket, sends a set_fft_size, and verifies the
// header of the next-arrived frame reflects the new size.
//
// Owns the backend's lifetime — every test must terminate its child
// process via the afterEach hook even on failure, otherwise port 8765
// (or the test's ephemeral port) stays bound across runs.

import { describe, it, expect, afterEach } from 'vitest';
import { spawn } from 'node:child_process';
import { existsSync } from 'node:fs';
import { createServer } from 'node:net';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
// Node 22's built-in WebSocket is RFC-strict and rejects our bundled
// SimpleWebSocket server's handshake response. `ws` is lenient about
// the same response and matches what Chromium does in production.
import WebSocket from 'ws';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const BACKEND_BIN = process.env.SIGNALSCOPE_BACKEND_BIN ??
  path.resolve(HERE, '../../build/backend-simple/signalscope-backend');

// Pick an OS-assigned ephemeral port; bind, capture, close — the
// 1-2ms gap before the backend binds is tolerable in CI and gives us
// a port that nothing else on the system is currently using.
async function pickPort() {
  return new Promise((resolve, reject) => {
    const server = createServer();
    server.on('error', reject);
    server.listen(0, '127.0.0.1', () => {
      const { port } = server.address();
      server.close(() => resolve(port));
    });
  });
}

// SIGTERM with a hard timeout escalation to SIGKILL. Returns once the
// child has actually exited so the afterEach hook releases its port
// before the next test starts.
async function killProcess(child, timeoutMs = 1500) {
  if (!child || child.exitCode != null) return;
  child.kill('SIGTERM');
  const start = Date.now();
  while (child.exitCode == null && Date.now() - start < timeoutMs) {
    await new Promise(r => setTimeout(r, 25));
  }
  if (child.exitCode == null) child.kill('SIGKILL');
  // Final small wait so the kernel finalises socket teardown.
  await new Promise(r => setTimeout(r, 50));
}

// Wait until the backend prints its "Listening on port" line. Without
// this we can hit a connection-refused window between spawn and bind.
async function waitForBackendReady(child, timeoutMs = 5000) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(
      () => reject(new Error('backend startup timeout')), timeoutMs);
    let buf = '';
    const onData = (chunk) => {
      buf += chunk.toString();
      if (/Listening on port/.test(buf)) {
        clearTimeout(timer);
        child.stdout.removeListener('data', onData);
        resolve();
      }
    };
    child.stdout.on('data', onData);
    child.once('exit', (code) => {
      clearTimeout(timer);
      reject(new Error(`backend exited before listening, code=${code}`));
    });
  });
}

describe('backend integration: settings sync', () => {
  let backend = null;
  let ws = null;

  afterEach(async () => {
    if (ws) {
      try { ws.close(); } catch {}
      ws = null;
    }
    await killProcess(backend);
    backend = null;
  });

  // The whole file no-ops gracefully if the binary isn't built yet.
  // Build with: npm run build:backend:simple-ws
  if (!existsSync(BACKEND_BIN)) {
    it.skip(
      `backend binary not found at ${BACKEND_BIN} — build it first`,
      () => {}
    );
    return;
  }

  it(
    'applies set_fft_size sent immediately on connect (race-condition regression)',
    async () => {
      const port = await pickPort();
      backend = spawn(
        BACKEND_BIN,
        [`--port=${port}`, '--test', '--fft=4096'],
        { stdio: ['ignore', 'pipe', 'pipe'] }
      );

      // Surface backend output if the test fails — silenced by default.
      const backendLog = [];
      backend.stdout.on('data', d => backendLog.push(['out', d.toString()]));
      backend.stderr.on('data', d => backendLog.push(['err', d.toString()]));

      try {
        await waitForBackendReady(backend);

        ws = new WebSocket(`ws://127.0.0.1:${port}`);
        await new Promise((resolve, reject) => {
          const t = setTimeout(() => reject(new Error('WS open timeout')), 3000);
          ws.once('open', () => { clearTimeout(t); resolve(); });
          ws.once('error', e => { clearTimeout(t); reject(e); });
        });

        // The bug we're guarding against: command sent right after open,
        // before any frame has arrived. Backend must process it in
        // command order and reflect it in the next emitted frame.
        ws.send(JSON.stringify({ cmd: 'set_fft_size', value: 1024 }));

        // Inspect the 16-byte header on every arriving frame; bytes
        // 4..5 are fft_size (uint16 LE), same as FrameHeader in
        // src/backend/protocol/protocol.hpp. Pass when we see 1024.
        const observed = await new Promise((resolve, reject) => {
          const t = setTimeout(
            () => reject(new Error('no frame with fft_size=1024 within 3s')),
            3000);
          const onMessage = (data) => {
            if (!(data instanceof Buffer)) return;
            if (data.length < 16) return;
            const size = data.readUInt16LE(4);
            if (size === 1024) {
              clearTimeout(t);
              ws.off('message', onMessage);
              resolve(size);
            }
          };
          ws.on('message', onMessage);
        });

        expect(observed).toBe(1024);
      } catch (err) {
        // Dump captured backend output to ease post-mortem.
        for (const [stream, line] of backendLog) {
          process.stderr.write(`[backend:${stream}] ${line}`);
        }
        throw err;
      }
    },
    15000
  );
});
