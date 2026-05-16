import { test, expect } from '@playwright/test';
import path from 'path';
import { launchApp, mockOpenFile, hookSentCommands, clickTestId } from './helpers.js';

const FIX = path.resolve(process.cwd(), 'tests/e2e/fixtures');

test('opening a .wav dispatches load_file without showing the modal', async ({}, testInfo) => {
  const { app, page } = await launchApp(testInfo);
  try {
    const getSent = await hookSentCommands(page);
    await mockOpenFile(app, {
      canceled: false,
      filePaths: [path.join(FIX, 'silence.wav')],
    });

    await clickTestId(page, 'sidebar-tab-input');
    await page.locator('[data-testid="source-select"] select').selectOption('file');
    await clickTestId(page, 'browse-file');

    await expect.poll(async () => {
      const sent = await getSent();
      return sent.some((s) => s.includes('"cmd":"load_file"'));
    }, { timeout: 5000 }).toBe(true);

    await expect(page.locator('[data-testid="file-load-modal"]')).toHaveCount(0);
  } finally {
    await app.close();
  }
});

test('opening a .raw file opens FileLoadModal; submit fires load_file', async ({}, testInfo) => {
  const { app, page } = await launchApp(testInfo);
  try {
    const getSent = await hookSentCommands(page);
    await mockOpenFile(app, {
      canceled: false,
      filePaths: [path.join(FIX, 'raw.bin')],
    });

    await clickTestId(page, 'sidebar-tab-input');
    await page.locator('[data-testid="source-select"] select').selectOption('file');
    await clickTestId(page, 'browse-file');

    await expect(page.locator('[data-testid="file-load-modal"]')).toBeVisible({ timeout: 5000 });

    await page.locator('[data-testid="file-load-samplerate"]').fill('48000');
    await page.locator('[data-testid="file-load-datatype"]').selectOption('rf32_le');
    await page.locator('[data-testid="file-load-channels"]').fill('1');

    await clickTestId(page, 'file-load-submit');

    await expect.poll(async () => {
      const sent = await getSent();
      return sent.find((s) => s.includes('"cmd":"load_file"') && s.includes('"rf32_le"')) ?? null;
    }, { timeout: 5000 }).not.toBeNull();

    await expect(page.locator('[data-testid="file-load-modal"]')).toHaveCount(0);
  } finally {
    await app.close();
  }
});

test('cancel button dismisses the modal without sending load_file', async ({}, testInfo) => {
  const { app, page } = await launchApp(testInfo);
  try {
    const getSent = await hookSentCommands(page);
    await mockOpenFile(app, {
      canceled: false,
      filePaths: [path.join(FIX, 'raw.bin')],
    });

    await clickTestId(page, 'sidebar-tab-input');
    await page.locator('[data-testid="source-select"] select').selectOption('file');
    await clickTestId(page, 'browse-file');

    await expect(page.locator('[data-testid="file-load-modal"]')).toBeVisible();
    await clickTestId(page, 'file-load-cancel');
    await expect(page.locator('[data-testid="file-load-modal"]')).toHaveCount(0);

    const sent = await getSent();
    expect(sent.find((s) => s.includes('"cmd":"load_file"'))).toBeUndefined();
  } finally {
    await app.close();
  }
});

test('SigMF with sidecar skips modal and dispatches load_file with sidecar values', async ({}, testInfo) => {
  const { app, page } = await launchApp(testInfo);
  try {
    const getSent = await hookSentCommands(page);
    await mockOpenFile(app, {
      canceled: false,
      filePaths: [path.join(FIX, 'sigmf-fixture.sigmf-data')],
    });

    await clickTestId(page, 'sidebar-tab-input');
    await page.locator('[data-testid="source-select"] select').selectOption('file');
    await clickTestId(page, 'browse-file');

    await expect(page.locator('[data-testid="file-load-modal"]')).toHaveCount(0, { timeout: 2000 });

    await expect.poll(async () => {
      const sent = await getSent();
      return sent.find((s) =>
        s.includes('"cmd":"load_file"') &&
        s.includes('"datatype":"cf32_le"') &&
        s.includes('"sample_rate":1000000') &&
        s.includes('"center_frequency":100000000')
      ) ?? null;
    }, { timeout: 5000 }).not.toBeNull();
  } finally {
    await app.close();
  }
});
