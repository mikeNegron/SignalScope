# E2E test suite

Every spec runs against the full FFT × WS backend matrix on each invocation:

| FFT \ WS    | uWS                   | simple-ws                  |
|-------------|-----------------------|----------------------------|
| pocketfft   | pocketfft-uws         | pocketfft-simple-ws        |
| fftw        | fftw-uws              | fftw-simple-ws             |
| fftw-static | fftw-static-uws       | fftw-static-simple-ws      |
| kfr         | kfr-uws               | kfr-simple-ws              |

## Running

`npm run test:e2e` builds all 8 backend variants (`build:backend:matrix`) and
then runs `playwright test`. Each project's binary path is held in
`testInfo.project.metadata.backendBinary` and propagated to Electron via
`SIGNALSCOPE_BACKEND_BIN`. Tests stay windowless via `SIGNALSCOPE_E2E=1`.

To iterate fast against a single variant during development:

    npm run build:backend:pocketfft:uws
    ELECTRON_RUN_AS_NODE= npx playwright test --project=pocketfft-uws

(`ELECTRON_RUN_AS_NODE=` unsets the shell-level `ELECTRON_RUN_AS_NODE=1` that
WSL exports; without it Electron launches in node mode and Playwright's
electron driver fails with "bad option: --no-sandbox".)

## Prerequisites

- `libfftw3f-dev` installed for the fftw and fftw-static variants.
- KFR vendored (see `setup.sh`) for the kfr variant.

Missing prerequisites cause the matching `build:backend:*` script to fail;
`playwright.config.js` warns and skips projects whose binary is absent so
you can still run the variants you do have.

## Status

All specs pass across the full 8-variant matrix. See
`docs/superpowers/plans/2026-05-14-backend-lifecycle-fixes.md` for the
lifecycle-bug fixes that closed the three previously-known follow-ups
(uWS-ignores-SIGTERM in the C++ backend, and a `start-backend` IPC
race in the Electron main process).
