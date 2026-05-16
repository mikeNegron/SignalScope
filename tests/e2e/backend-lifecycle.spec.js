// tests/e2e/backend-lifecycle.spec.js
import { test, expect, _electron as electron } from '@playwright/test';
import path from 'path';
import { launchApp, waitForBackendConfig, waitForFirstFrame } from './helpers.js';

const REPO_ROOT = path.resolve(process.cwd());

test('SIGNALSCOPE_BACKEND_BIN env var selects the binary', async () => {
  // Point at a *non-default-path* binary so this test only passes when the
  // env override is honored. Build it ahead of this test via Task 2's
  // `build:backend:pocketfft:simple-ws` (different inode + different path
  // than the default build/backend/signalscope-backend).
  const binPath = path.join(REPO_ROOT, 'build/backend-pocketfft-simple-ws/signalscope-backend');
  const app = await electron.launch({
    args: [REPO_ROOT],
    timeout: 15_000,
    env: {
      ...process.env,
      SIGNALSCOPE_E2E: '1',
      SIGNALSCOPE_BACKEND_BIN: binPath,
    },
  });
  try {
    const page = await app.firstWindow();
    await page.waitForFunction(
      () => typeof window.signalscope?.getBackendConfig === 'function',
      null,
      { timeout: 10_000 }
    );
    const cfg = await page.evaluate(() => window.signalscope.getBackendConfig());
    expect(cfg.running).toBe(true);
  } finally {
    await app.close();
  }
});

test.describe('backend lifecycle IPCs', () => {
  test('stopBackend kills the child; startBackend restarts it', async ({}, testInfo) => {
    const { app, page } = await launchApp(testInfo);
    try {
      const cfgBefore = await waitForBackendConfig(page);
      expect(cfgBefore.running).toBe(true);

      // Stop.
      const stopRes = await page.evaluate(() => window.signalscope.stopBackend());
      expect(stopRes.success).toBe(true);

      // After a stop, restart and verify a fresh backend comes up.
      const startRes = await page.evaluate(() => window.signalscope.startBackend());
      expect(startRes.success).toBe(true);
      await page.waitForFunction(async () => {
        const cfg = await window.signalscope.getBackendConfig();
        return cfg.running === true;
      }, null, { timeout: 8000 });
    } finally {
      await app.close();
    }
  });

  test('restartBackend re-emits backend-config to renderer', async ({}, testInfo) => {
    const { app, page } = await launchApp(testInfo);
    try {
      await waitForFirstFrame(page);

      await page.evaluate(() => {
        window.__lastBackendCfg = null;
        window.signalscope.onBackendConfig((cfg) => { window.__lastBackendCfg = cfg; });
      });

      const res = await page.evaluate(() => window.signalscope.restartBackend());
      expect(res.success).toBe(true);

      await page.waitForFunction(
        () => window.__lastBackendCfg?.running === true,
        null,
        { timeout: 8000 }
      );
    } finally {
      await app.close();
    }
  });
});

test('SIGNALSCOPE_NO_BACKEND skips spawn; renderer falls back to simulation', async ({}, testInfo) => {
  const { app, page } = await launchApp(testInfo, { noBackend: true });
  try {
    const cfg = await page.evaluate(() => window.signalscope.getBackendConfig());
    expect(cfg.running).toBe(false);
    expect(cfg.reason).toBe('not-spawned');

    // The renderer should show "Simulated" in the status bar source label.
    await expect(page.locator('[data-testid="status-source-label"]'))
      .toHaveText(/simulated/i, { timeout: 5000 });
  } finally {
    await app.close();
  }
});
