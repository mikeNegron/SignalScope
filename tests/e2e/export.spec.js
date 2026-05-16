import { test, expect } from '@playwright/test';
import path from 'path';
import os from 'os';
import fs from 'fs';
import { launchApp, waitForFirstFrame, mockSaveFile, hookSentCommands, clickTestId } from './helpers.js';

// Export buttons live on the Settings tab (not Peaks). The CSV export goes
// through window.signalscope.saveFile → cmd("export_capture", {format:"csv", ...}).
test('export CSV dispatches saveFile + export_capture command', async ({}, testInfo) => {
  const tmp = path.join(os.tmpdir(), `signalscope-export-${Date.now()}.csv`);
  const { app, page } = await launchApp(testInfo);
  try {
    await waitForFirstFrame(page);
    await mockSaveFile(app, tmp);
    const getSent = await hookSentCommands(page);

    await clickTestId(page, 'sidebar-tab-settings');
    await clickTestId(page, 'export-csv');

    await expect.poll(async () => {
      const sent = await getSent();
      return sent.find((s) =>
        s.includes('"cmd":"export_capture"') &&
        s.includes('"format":"csv"')
      ) ?? null;
    }, { timeout: 5000 }).not.toBeNull();
  } finally {
    await app.close();
    if (fs.existsSync(tmp)) fs.unlinkSync(tmp);
  }
});
