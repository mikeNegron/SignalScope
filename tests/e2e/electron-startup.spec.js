// End-to-end startup tests for the Electron + backend integration.
//
// Covers Plan A Task 4's IPC contract: `get-backend-config` must return
// { running: true, port } only after the backend prints
// "[ws] Listening on port N", and { running: false, reason } when the
// backend exits with "[ws] fatal:" or fails to listen within 5 s.

import { test, expect, _electron as electron } from '@playwright/test';
import net from 'net';
import path from 'path';

const REPO_ROOT = path.resolve(process.cwd());

// Helper: launch the Electron app and wait for its first BrowserWindow.
// Returns { app, page } where page is the renderer's main window.
async function launchApp() {
  const app = await electron.launch({
    args: [REPO_ROOT],            // run `electron .` from the repo root
    timeout: 15_000,
    // Tell main.js to create the BrowserWindow with show:false so the
    // window doesn't flash onto the developer's desktop on every run.
    env: { ...process.env, SIGNALSCOPE_E2E: '1' },
  });
  const page = await app.firstWindow();
  // Wait until the renderer has loaded enough JS to expose the bridge.
  await page.waitForFunction(
    () => typeof window.signalscope?.getBackendConfig === 'function',
    null,
    { timeout: 10_000 }
  );
  return { app, page };
}

// Helper: poll the IPC until it returns a definitive result (running
// true OR running false with a reason). The IPC awaits backendReadyPromise
// internally so the first call already blocks for up to 5 s; we add a
// modest outer poll budget for renderer-side timing slack.
async function waitForBackendConfig(page, maxMs = 8000) {
  const start = Date.now();
  while (Date.now() - start < maxMs) {
    const cfg = await page.evaluate(() => window.signalscope.getBackendConfig());
    if (cfg && (cfg.running === true || cfg.reason != null)) return cfg;
    await new Promise((r) => setTimeout(r, 100));
  }
  throw new Error('backend config did not settle within ' + maxMs + 'ms');
}

test.describe('Electron startup IPC contract', () => {
  test('happy path: backend reports running with a port', async () => {
    const { app, page } = await launchApp();
    try {
      const cfg = await waitForBackendConfig(page);
      expect(cfg.running).toBe(true);
      expect(typeof cfg.port).toBe('number');
      expect(cfg.port).toBeGreaterThan(0);
    } finally {
      await app.close();
    }
  });

  test('fatal path: backend reports not running with a fatal reason', async () => {
    // Hold port 8765 BEFORE launching so the backend's bind fails.
    const holder = net.createServer().listen(8765);
    await new Promise((r) => holder.once('listening', r));

    let app, page;
    try {
      ({ app, page } = await launchApp());
      const cfg = await waitForBackendConfig(page);
      expect(cfg.running).toBe(false);
      expect(cfg.reason).toBeDefined();
      expect(String(cfg.reason).toLowerCase()).toContain('fatal:');
    } finally {
      if (app) await app.close();
      holder.close();
    }
  });
});
