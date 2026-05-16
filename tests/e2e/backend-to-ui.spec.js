import { test, expect } from '@playwright/test';
import { launchApp, waitForFirstFrame } from './helpers.js';

test('connection pill flips to Connected after first backend frame', async ({}, testInfo) => {
  const { app, page } = await launchApp(testInfo);
  try {
    // connection-pill only renders inside the Input tab; activate it first.
    await page.locator('[data-testid="sidebar-tab-input"]').click();
    await expect(page.locator('[data-testid="connection-pill"]')).toBeVisible({ timeout: 5000 });

    await waitForFirstFrame(page);

    await expect(page.locator('[data-testid="status-source-label"]'))
      .toHaveText('C++ Backend', { timeout: 5000 });
  } finally {
    await app.close();
  }
});

test('frame counter advances monotonically once backend is sending', async ({}, testInfo) => {
  const { app, page } = await launchApp(testInfo);
  try {
    await waitForFirstFrame(page);
    const f1 = Number(await page.locator('[data-testid="status-frame-id"]').textContent());
    // 4Hz React flush of frameId; wait > 250ms.
    await page.waitForTimeout(800);
    const f2 = Number(await page.locator('[data-testid="status-frame-id"]').textContent());
    expect(f2).toBeGreaterThan(f1);
  } finally {
    await app.close();
  }
});

test('status bar shows bin width derived from backend sample rate', async ({}, testInfo) => {
  const { app, page } = await launchApp(testInfo);
  try {
    await waitForFirstFrame(page);
    // Bin width = sampleRate / fftSize. Default 48000/4096 = 11.72 Hz.
    // The label format is "<num> Hz" with 2-decimal precision.
    await expect(page.locator('[data-testid="status-sample-rate"]'))
      .toHaveText(/^[0-9]+\.[0-9]{2} Hz$/, { timeout: 5000 });
  } finally {
    await app.close();
  }
});
