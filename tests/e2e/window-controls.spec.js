import { test, expect } from '@playwright/test';
import { launchApp, clickTestId } from './helpers.js';

test.describe('window controls', () => {
  test('minimize button calls win.minimize via IPC', async ({}, testInfo) => {
    const { app, page } = await launchApp(testInfo);
    try {
      // Stub the main-process minimize so we can detect the call without
      // actually minimizing (which is no-op when show:false anyway).
      const calledHandle = await app.evaluate(({ BrowserWindow }) => {
        globalThis.__minimizeCalls = 0;
        const win = BrowserWindow.getAllWindows()[0];
        const orig = win.minimize.bind(win);
        win.minimize = () => { globalThis.__minimizeCalls++; orig(); };
        return true;
      });
      expect(calledHandle).toBe(true);
      await clickTestId(page, 'titlebar-minimize');
      // Poll until the call is recorded — IPC is async.
      await expect.poll(() =>
        app.evaluate(() => globalThis.__minimizeCalls)
      ).toBeGreaterThanOrEqual(1);
    } finally {
      await app.close();
    }
  });

  test('close button quits the app', async ({}, testInfo) => {
    const { app, page } = await launchApp(testInfo);
    let appClosed = false;
    app.on('close', () => { appClosed = true; });
    await clickTestId(page, 'titlebar-close');
    // The app handles `window-all-closed` and quits the process.
    await app.waitForEvent('close', { timeout: 5000 });
    expect(appClosed).toBe(true);
  });

  test('toggle-maximize fires window:state-change and updates titlebar', async ({}, testInfo) => {
    const { app, page } = await launchApp(testInfo);
    try {
      // Initial state: not maximized.
      await expect(page.locator('[data-testid="titlebar-root"]'))
        .toHaveAttribute('data-maximized', '0');

      await clickTestId(page, 'titlebar-maximize');

      await expect(page.locator('[data-testid="titlebar-root"]'))
        .toHaveAttribute('data-maximized', '1', { timeout: 5000 });

      // Toggle back.
      await clickTestId(page, 'titlebar-maximize');
      await expect(page.locator('[data-testid="titlebar-root"]'))
        .toHaveAttribute('data-maximized', '0', { timeout: 5000 });
    } finally {
      await app.close();
    }
  });
});
