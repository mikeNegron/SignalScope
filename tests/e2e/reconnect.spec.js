import net from 'net';
import { test, expect } from '@playwright/test';
import { launchApp, waitForFirstFrame } from './helpers.js';

test('killing backend mid-session flips renderer to Disconnected, then reconnects after restart', async ({}, testInfo) => {
  const { app, page } = await launchApp(testInfo);
  try {
    await waitForFirstFrame(page);
    await expect(page.locator('[data-testid="status-source-label"]'))
      .toHaveText('C++ Backend');

    // Stop the backend via the public IPC.
    await page.evaluate(() => window.signalscope.stopBackend());

    // The renderer's ws onclose drops connection — __debug_connected goes false.
    await page.waitForFunction(() => window.__debug_connected === false, null, { timeout: 5000 });

    // Restart and confirm reconnect.
    await page.evaluate(() => window.signalscope.startBackend());
    await page.waitForFunction(() => window.__debug_connected === true, null, { timeout: 10_000 });
  } finally {
    await app.close();
  }
});

test('backend fatal "port in use" surfaces in renderer via getBackendConfig', async ({}, testInfo) => {
  const holder = net.createServer().listen(8765);
  await new Promise((r) => holder.once('listening', r));

  try {
    const { app, page } = await launchApp(testInfo);
    try {
      const cfg = await page.evaluate(() => window.signalscope.getBackendConfig());
      expect(cfg.running).toBe(false);
      expect(String(cfg.reason).toLowerCase()).toContain('fatal:');
      await expect(page.locator('[data-testid="status-source-label"]'))
        .toHaveText(/simulated/i, { timeout: 5000 });
    } finally {
      await app.close();
    }
  } finally {
    holder.close();
  }
});
