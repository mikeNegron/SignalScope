import { test, expect } from '@playwright/test';
import { launchApp } from './helpers.js';

const EXPECTED = [
  'getBackendConfig', 'restartBackend', 'startBackend', 'stopBackend',
  'minimizeWindow', 'closeWindow', 'toggleMaximizeWindow',
  'onBackendConfig', 'onWindowStateChange',
  'openFile', 'saveFile',
  'platform',
];

test('preload exposes the documented surface and nothing else', async ({}, testInfo) => {
  const { app, page } = await launchApp(testInfo);
  try {
    const surface = await page.evaluate(() => {
      const out = {};
      for (const k of Object.keys(window.signalscope)) {
        out[k] = typeof window.signalscope[k];
      }
      return out;
    });
    for (const key of EXPECTED) {
      expect(surface).toHaveProperty(key);
    }
    expect(Object.keys(surface).sort()).toEqual([...EXPECTED].sort());
    expect(surface.platform).toBe('string');
    expect(surface.getBackendConfig).toBe('function');
  } finally {
    await app.close();
  }
});
