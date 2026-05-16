import { test, expect } from '@playwright/test';
import { launchApp, clickTestId, hookSentCommands } from './helpers.js';

const TABS = ['measure', 'cursors', 'peaks', 'input', 'settings'];

test('clicking each sidebar tab updates the persisted state', async ({}, testInfo) => {
  const { app, page } = await launchApp(testInfo);
  try {
    for (const t of TABS) {
      await clickTestId(page, `sidebar-tab-${t}`);
      // The persisted-state write is coalesced through requestAnimationFrame,
      // so poll for the value rather than reading once immediately after click.
      await expect.poll(async () => {
        return await page.evaluate(() =>
          window.localStorage.getItem('signalscope.settings.sidebarTab')
        );
      }, { timeout: 2000 }).toContain(`"d":"${t}"`);
    }
  } finally {
    await app.close();
  }
});

test('selecting "test" source fires set_source("test")', async ({}, testInfo) => {
  const { app, page } = await launchApp(testInfo);
  try {
    const getSent = await hookSentCommands(page);
    await clickTestId(page, 'sidebar-tab-input');
    await page.locator('[data-testid="source-select"] select').selectOption('test');
    await expect.poll(async () => {
      const sent = await getSent();
      return sent.find((s) =>
        s.includes('"cmd":"set_source"') && s.includes('"value":"test"')
      ) ?? null;
    }, { timeout: 3000 }).not.toBeNull();
  } finally {
    await app.close();
  }
});

test('selecting "websocket" does NOT fire set_source (UI-only label)', async ({}, testInfo) => {
  const { app, page } = await launchApp(testInfo);
  try {
    await clickTestId(page, 'sidebar-tab-input');
    const getSent = await hookSentCommands(page);
    await page.locator('[data-testid="source-select"] select').selectOption('test');
    await page.waitForTimeout(200);
    await page.locator('[data-testid="source-select"] select').selectOption('websocket');
    await page.waitForTimeout(400);
    const sent = await getSent();
    const setSourceWs = sent.find((s) =>
      s.includes('"cmd":"set_source"') && s.includes('"value":"websocket"')
    );
    expect(setSourceWs).toBeUndefined();
  } finally {
    await app.close();
  }
});

test('Listen source: editing port emits set_listen_port', async ({}, testInfo) => {
  const { app, page } = await launchApp(testInfo);
  try {
    await clickTestId(page, 'sidebar-tab-input');
    await page.locator('[data-testid="source-select"] select').selectOption('listen');

    const getSent = await hookSentCommands(page);
    const portField = page.locator('[data-testid="listen-port"] input');
    await portField.fill('');
    await portField.fill('6000');
    await portField.blur();

    await expect.poll(async () => {
      const sent = await getSent();
      return sent.find((s) =>
        s.includes('"cmd":"set_listen_port"') && s.includes('"value":6000')
      ) ?? null;
    }, { timeout: 3000 }).not.toBeNull();
  } finally {
    await app.close();
  }
});
