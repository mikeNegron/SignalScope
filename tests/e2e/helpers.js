// Shared helpers for Electron E2E specs.
// - launchApp({ noBackend? }) launches Electron windowless, with the
//   binary selected by the current Playwright project's metadata. Returns
//   { app, page } where page.signalscope... is the preload-exposed bridge.
// - waitForBackendConfig polls the IPC until { running: true|false } settles.
// - waitForFirstFrame waits until the renderer has received at least one
//   data frame (i.e., backend pill flips to Connected).
// - mockOpenFile / mockSaveFile override dialog.showOpenDialog and
//   dialog.showSaveDialog inside the main process for a single test run.

import { _electron as electron, test as base } from '@playwright/test';
import path from 'path';

const REPO_ROOT = path.resolve(process.cwd());

export function backendBinaryForProject(testInfo) {
  // Playwright forwards the project's metadata through testInfo.project.metadata.
  return testInfo.project.metadata.backendBinary;
}

export async function launchApp(testInfo, { noBackend = false, env = {} } = {}) {
  const binary = backendBinaryForProject(testInfo);
  const baseEnv = {
    ...process.env,
    SIGNALSCOPE_E2E: '1',
    SIGNALSCOPE_BACKEND_BIN: binary,
    ...env,
  };
  if (noBackend) baseEnv.SIGNALSCOPE_NO_BACKEND = '1';

  const app = await electron.launch({
    args: [REPO_ROOT],
    timeout: 15_000,
    env: baseEnv,
  });
  const page = await app.firstWindow();
  await page.waitForFunction(
    () => typeof window.signalscope?.getBackendConfig === 'function',
    null,
    { timeout: 10_000 }
  );
  return { app, page };
}

export async function waitForBackendConfig(page, maxMs = 8000) {
  const start = Date.now();
  while (Date.now() - start < maxMs) {
    const cfg = await page.evaluate(() => window.signalscope.getBackendConfig());
    if (cfg && (cfg.running === true || cfg.reason != null)) return cfg;
    await new Promise((r) => setTimeout(r, 100));
  }
  throw new Error('backend config did not settle within ' + maxMs + 'ms');
}

export async function waitForFirstFrame(page, maxMs = 8000) {
  // window.__debug_connected is set from index.jsx after the first frame.
  await page.waitForFunction(() => window.__debug_connected === true, null, { timeout: maxMs });
}

// Mock dialog.showOpenDialog in the main process. Returns a teardown fn that
// restores the original handler. The override returns `result` exactly once,
// then falls back to "canceled" so a stuck dialog can't hang the test.
export async function mockOpenFile(app, result) {
  await app.evaluate(({ dialog }, fakeResult) => {
    if (!globalThis.__origShowOpenDialog) {
      globalThis.__origShowOpenDialog = dialog.showOpenDialog;
    }
    let used = false;
    dialog.showOpenDialog = async () => {
      if (used) return { canceled: true, filePaths: [] };
      used = true;
      return fakeResult;
    };
  }, result);
}

export async function mockSaveFile(app, savePath) {
  await app.evaluate(({ dialog }, fakePath) => {
    if (!globalThis.__origShowSaveDialog) {
      globalThis.__origShowSaveDialog = dialog.showSaveDialog;
    }
    let used = false;
    dialog.showSaveDialog = async () => {
      if (used) return { canceled: true };
      used = true;
      return { canceled: false, filePath: fakePath };
    };
  }, savePath);
}

// Page-level helper: click an element by data-testid.
export async function clickTestId(page, id) {
  await page.locator(`[data-testid="${id}"]`).click();
}

export async function getTestIdText(page, id) {
  return (await page.locator(`[data-testid="${id}"]`).first().textContent())?.trim() ?? '';
}

// Capture WebSocket messages sent FROM the renderer. Hooks the WebSocket
// constructor in the page so each spec can assert the cmd traffic produced
// by a UI interaction (e.g., "clicking Listen → backend gets set_source").
// Returns an async function `getSent()` returning the array of message
// strings since hookSentCommands was called.
export async function hookSentCommands(page) {
  await page.evaluate(() => {
    window.__sentCmds = [];
    // Patch WebSocket.prototype.send so BOTH existing live sockets (constructed
    // before this hook ran, e.g. the renderer's backend WS) AND any future
    // sockets record their outgoing payloads. Wrapping the constructor alone
    // misses sockets created during module init.
    const proto = window.WebSocket.prototype;
    if (!proto.__sendHooked) {
      const origSend = proto.send;
      proto.send = function (data) {
        try { window.__sentCmds.push(data); } catch (_) {}
        return origSend.call(this, data);
      };
      proto.__sendHooked = true;
    }
  });
  return async () => page.evaluate(() => window.__sentCmds.slice());
}
