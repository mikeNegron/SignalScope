import { test, expect } from '@playwright/test';
import { launchApp, waitForFirstFrame, hookSentCommands } from './helpers.js';

test('spacebar toggles pause and sends set_pause', async ({}, testInfo) => {
  const { app, page } = await launchApp(testInfo);
  try {
    await waitForFirstFrame(page);
    const getSent = await hookSentCommands(page);

    await page.keyboard.press('Space');
    await expect.poll(async () => {
      const sent = await getSent();
      return sent.find((s) => s.includes('"cmd":"set_pause"') && s.includes('"value":true')) ?? null;
    }, { timeout: 3000 }).not.toBeNull();

    await page.keyboard.press('Space');
    await expect.poll(async () => {
      const sent = await getSent();
      return sent.find((s) => s.includes('"cmd":"set_pause"') && s.includes('"value":false')) ?? null;
    }, { timeout: 3000 }).not.toBeNull();
  } finally {
    await app.close();
  }
});

test('spacebar inside an input field is suppressed (does not pause)', async ({}, testInfo) => {
  const { app, page } = await launchApp(testInfo);
  try {
    await waitForFirstFrame(page);
    // Open Input tab so the sample-rate NumInput is visible.
    await page.locator('[data-testid="sidebar-tab-input"]').click();
    const getSent = await hookSentCommands(page);

    // Focus the first numeric input on the Input tab.
    const srInput = page.locator('input[type="number"]').first();
    await srInput.focus();
    await page.keyboard.press('Space');

    await page.waitForTimeout(400);
    const sent = await getSent();
    expect(sent.find((s) => s.includes('"cmd":"set_pause"'))).toBeUndefined();
  } finally {
    await app.close();
  }
});
