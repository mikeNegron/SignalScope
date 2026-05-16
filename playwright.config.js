// Playwright config for end-to-end Electron tests. Every spec runs against
// the full FFT × WS backend matrix (4 × 2 = 8 variants) so we never let any
// (fft, ws) combo silently diverge — extends the feedback_backend_testing_matrix
// rule from {pocketfft, simple-ws, uws} to the full cross product.

import { defineConfig } from '@playwright/test';
import path from 'path';
import fs from 'fs';

const REPO_ROOT = path.resolve(process.cwd());

const FFT_LIBS = ['pocketfft', 'fftw', 'fftw-static', 'kfr'];
const WS_IMPLS = ['uws', 'simple-ws'];

// Build the cross product. We only include projects whose binary has been
// built — that way a developer who hasn't installed libfftw3 / KFR can still
// run the suite against the available variants. CI must build all 8 (see
// npm run build:backend:matrix in test:e2e).
const allCombos = FFT_LIBS.flatMap((fft) =>
  WS_IMPLS.map((ws) => ({
    name: `${fft}-${ws}`,
    binary: path.join(REPO_ROOT, `build/backend-${fft}-${ws}/signalscope-backend`),
  }))
);

const availableCombos = allCombos.filter(({ binary }) => {
  if (fs.existsSync(binary)) return true;
  console.warn(`[playwright] skipping project (binary missing): ${binary}`);
  return false;
});

if (availableCombos.length === 0) {
  throw new Error(
    'No backend binaries found. Run `npm run build:backend:matrix` first.'
  );
}

export default defineConfig({
  testDir: './tests/e2e',
  fullyParallel: false,            // Each test launches a full Electron app.
  timeout: 30_000,
  expect: { timeout: 10_000 },
  reporter: 'list',
  workers: 1,
  projects: availableCombos.map(({ name, binary }) => ({
    name,
    metadata: { backendBinary: binary },
  })),
});
