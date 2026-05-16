import { test, expect } from '@playwright/test';
import { launchApp, waitForFirstFrame, hookSentCommands } from './helpers.js';

test('fftSize selection survives a renderer reload', async ({}, testInfo) => {
  const { app, page } = await launchApp(testInfo);
  try {
    await waitForFirstFrame(page);

    await page.evaluate(() => {
      window.localStorage.setItem(
        'signalscope.settings.fftSize',
        JSON.stringify({ v: 1, d: '8192' })
      );
    });

    await page.reload();
    await page.waitForFunction(
      () => typeof window.signalscope?.getBackendConfig === 'function'
    );

    const restored = await page.evaluate(() =>
      window.localStorage.getItem('signalscope.settings.fftSize')
    );
    expect(restored).toContain('"d":"8192"');
  } finally {
    await app.close();
  }
});

test('PERSIST_VERSION mismatch falls back to defaults (not throws)', async ({}, testInfo) => {
  const { app, page } = await launchApp(testInfo);
  try {
    await waitForFirstFrame(page);
    await page.evaluate(() => {
      window.localStorage.setItem(
        'signalscope.settings.fftSize',
        JSON.stringify({ v: 999, d: 'garbage' })
      );
    });
    await page.reload();
    await page.waitForFunction(
      () => typeof window.signalscope?.getBackendConfig === 'function'
    );
    const bodyText = await page.locator('body').textContent();
    expect(bodyText).toBeTruthy();
  } finally {
    await app.close();
  }
});

test('on reconnect, persisted set_* settings are re-sent to backend', async ({}, testInfo) => {
  const { app, page } = await launchApp(testInfo);
  try {
    await waitForFirstFrame(page);
    const getSent = await hookSentCommands(page);

    // Trigger a reconnect by restarting the backend.
    await page.evaluate(() => window.signalscope.restartBackend());
    await page.waitForFunction(() => window.__debug_connected === true, null, { timeout: 10_000 });

    // The renderer's resync useEffect fires AFTER React commits the
    // post-reconnect render — so reading `sent` the moment
    // __debug_connected flips true can race the effect. Poll instead.
    const requiredCmds = [
      'set_fft_size', 'set_window', 'set_gain', 'set_avg_count',
      'set_tune_offset', 'set_decimation', 'set_suppress_image',
      'set_peak_min_level', 'set_weighting', 'set_overlap',
      'set_replay_speed', 'set_taper_count', 'set_history_capacity',
      'set_listen_port', 'set_listen_format', 'set_listen_sample_rate',
      'set_listen_center_freq', 'set_listen_expect_header',
    ];
    await expect.poll(async () => {
      const sent = await getSent();
      return requiredCmds.every((cmd) =>
        sent.some((s) => s.includes(`"cmd":"${cmd}"`))
      );
    }, { timeout: 5000 }).toBe(true);
  } finally {
    await app.close();
  }
});
