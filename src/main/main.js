// Force Mesa to use the D3D12 GPU driver instead of llvmpipe on WSL2
if (process.platform === "linux" && process.env.WSL_DISTRO_NAME) {
  process.env.MESA_D3D12_DEFAULT_ADAPTER_NAME = "";
  process.env.MESA_LOADER_DRIVER_OVERRIDE = "d3d12";
  process.env.LIBGL_DRIVERS_PATH = "/usr/lib/x86_64-linux-gnu/dri";
}

const { app, BrowserWindow, ipcMain, screen, dialog } = require("electron");
const path = require("path");
const { spawn } = require("child_process");
const fs = require("fs");

function getBackendPath() {
  // Explicit override (used by E2E tests to target a specific backend variant).
  if (process.env.SIGNALSCOPE_BACKEND_BIN) {
    return process.env.SIGNALSCOPE_BACKEND_BIN;
  }
  const isPackaged = app.isPackaged;
  const ext = process.platform === "win32" ? ".exe" : "";
  const binaryName = `signalscope-backend${ext}`;

  if (isPackaged) {
    return path.join(process.resourcesPath, "backend", binaryName);
  } else {
    return path.join(__dirname, "..", "..", "build", "backend", binaryName);
  }
}

// Resolve a user-picked file path to:
//   { path, format, hint, needsParams }
// hint pre-fills params known from the file or its sidecar (null otherwise);
// needsParams asks the renderer to prompt before sending load_file.
function resolveSignalFile(filePath) {
    const lower = filePath.toLowerCase();
    let format = null;
    if (lower.endsWith(".wav")) format = "wav";
    else if (lower.endsWith(".aiff") || lower.endsWith(".aif")) format = "aiff";
    else if (lower.endsWith(".f32")) format = "f32";
    else if (lower.endsWith(".raw")) format = "raw";
    else if (lower.endsWith(".sigmf-data")) format = "sigmf";
    else format = "raw"; // unknown extension -> treat as raw, force prompt

    // Self-describing formats - backend reads metadata from the file header.
    if (format === "wav" || format === "aiff") {
        return { path: filePath, format, hint: null, needsParams: false };
    }

    // .f32 has a conventional default (float32 LE, mono, real). Backend will
    // accept it without a datatype; renderer can confirm via the modal if it
    // wants, but we return needsParams=false so a "convenience" load works.
    if (format === "f32") {
        return {
            path: filePath,
            format,
            hint: { datatype: "rf32_le", sample_rate: 48000, channels: 1, complex: false },
            needsParams: true, // ask anyway - sample rate isn't known
        };
    }

    // SigMF: look for the metadata sidecar.
    if (format === "sigmf") {
        const meta = loadSigmfMeta(filePath);
        if (meta) {
            return { path: filePath, format, hint: meta, needsParams: false };
        }
        return { path: filePath, format, hint: null, needsParams: true };
    }

    // .raw - no defaults; prompt with empty fields.
    return { path: filePath, format, hint: null, needsParams: true };
}

// SigMF metadata is a JSON sidecar at `<basename>.sigmf-meta` (the spec).
// Some tools also use `.sigmf-meta.json`; support both. Returns a hint object
// or null if the sidecar is missing/invalid.
function loadSigmfMeta(dataPath) {
    const candidates = [
        dataPath.replace(/\.sigmf-data$/i, ".sigmf-meta"),
        dataPath.replace(/\.sigmf-data$/i, ".sigmf-meta.json"),
    ];
    let metaPath = null;
    for (const c of candidates) {
        if (fs.existsSync(c)) { metaPath = c; break; }
    }
    if (!metaPath) return null;
    try {
        const raw = fs.readFileSync(metaPath, "utf8");
        const meta = JSON.parse(raw);
        const g = meta.global || {};
        const datatype = g["core:datatype"];
        const sample_rate = g["core:sample_rate"];
        const num_channels = g["core:num_channels"];
        // SigMF puts center frequency on per-capture segments; for static-RF
        // recordings there's typically a single capture with the tuner freq.
        const captures = Array.isArray(meta.captures) ? meta.captures : [];
        const cap0 = captures[0] || {};
        const center_freq =
            cap0["core:frequency"] ?? g["core:frequency"] ?? 0;
        if (!datatype) return null;
        return {
            datatype,
            sample_rate: sample_rate ? Number(sample_rate) : 48000,
            channels:    num_channels ? Number(num_channels) : 1,
            complex:     /^c/.test(datatype),
            center_frequency: Number(center_freq) || 0,
        };
    } catch (e) {
        console.error(`[sigmf] failed to parse ${metaPath}: ${e.message}`);
        return null;
    }
}

let backendProcess = null;
let backendPort = 8765;

// Deferred Promise that resolves with { ready: true, port } once the
// backend prints "[ws] Listening on port N", or { ready: false, reason }
// if "[ws] fatal:" appears or the process exits / times out.
let backendReadyPromise = Promise.resolve({ ready: false, reason: "not-spawned" });
let backendReadyResolve = null;
let backendReadyTimer = null;
const BACKEND_READY_TIMEOUT_MS = 5000;

function resetBackendReadyPromise() {
  // Clear any stale timer from a prior spawn cycle so it can't fire
  // against the new Promise and clobber an otherwise-successful spawn.
  if (backendReadyTimer) {
    clearTimeout(backendReadyTimer);
    backendReadyTimer = null;
  }
  backendReadyPromise = new Promise((resolve) => {
    backendReadyResolve = resolve;
  });
}

function settleBackendReady(result) {
  if (backendReadyResolve) {
    if (backendReadyTimer) {
      clearTimeout(backendReadyTimer);
      backendReadyTimer = null;
    }
    backendReadyResolve(result);
    backendReadyResolve = null;
  }
}

function startBackend() {
  const backendPath = getBackendPath();

  if (!fs.existsSync(backendPath)) {
    console.error(`Backend binary not found at: ${backendPath}`);
    console.error("Build it with: npm run build:backend");
    return false;
  }

  console.log(`Starting backend: ${backendPath}`);

  // Arm the readiness Promise BEFORE spawning so any early stdout/stderr
  // chunks from the child have something to resolve into.
  resetBackendReadyPromise();
  backendReadyTimer = setTimeout(() => {
    settleBackendReady({ ready: false, reason: "timeout" });
  }, BACKEND_READY_TIMEOUT_MS);

  backendProcess = spawn(backendPath, [`--port=${backendPort}`], {
    stdio: ["ignore", "pipe", "pipe"],
    env: { ...process.env },
  });

  backendProcess.stdout.on("data", (data) => {
    const msg = data.toString().trim();
    console.log(`${msg}`);

    // Parse port from backend stdout if it reports one
    const portMatch = msg.match(/listening on port (\d+)/i);
    if (portMatch) {
      backendPort = parseInt(portMatch[1], 10);
      // Push backend-config immediately so the renderer connects without
      // waiting for its next retry cycle.
      if (mainWindow && !mainWindow.isDestroyed()) {
        mainWindow.webContents.send("backend-config", {
          wsUrl: `ws://localhost:${backendPort}`,
          port: backendPort,
          running: true,
        });
      }
    }

    // Resolve the readiness Promise once the backend confirms it's bound
    // to the port. The "[ws] Listening on port" line is emitted from
    // backend/main.cpp for both uWS and simple-WS code paths.
    if (msg.includes("[ws] Listening on port") && backendReadyResolve) {
      settleBackendReady({ ready: true, port: backendPort });
    }
  });

  backendProcess.stderr.on("data", (data) => {
    const text = data.toString();
    console.error(`[backend:err] ${text.trim()}`);

    // A "[ws] fatal:" line means listen()/bind() failed; the backend
    // will shortly exit with ws_fatal. Settle the Promise now so the
    // renderer gets a fast "not running" answer rather than waiting
    // the full 5 s timeout.
    if (text.includes("[ws] fatal:") && backendReadyResolve) {
      settleBackendReady({ ready: false, reason: "fatal: " + text.trim() });
    }
  });

  backendProcess.on("exit", (code, signal) => {
    console.log(`Backend exited: code=${code} signal=${signal}`);
    // If we never saw a "Listening" line, the process died before binding
    // its port - surface that to the IPC handler rather than hanging.
    if (backendReadyResolve) {
      settleBackendReady({
        ready: false,
        reason: `exited-before-listening (code ${code})`,
      });
    }
    backendProcess = null;
  });

  backendProcess.on("error", (err) => {
    console.error("Failed to start backend:", err.message);
    if (backendReadyResolve) {
      settleBackendReady({ ready: false, reason: `spawn-error: ${err.message}` });
    }
    backendProcess = null;
  });

  return true;
}

function stopBackend() {
  if (!backendProcess) return Promise.resolve();
  console.log("[ss] Stopping backend...");
  // Resolve when the process actually exits. The existing
  // backendProcess.on("exit", ...) handler (inside startBackend)
  // nulls backendProcess on exit — we just await the OS event.
  const proc = backendProcess;
  const done = new Promise((resolve) => proc.once("exit", resolve));
  proc.kill("SIGTERM");
  // SIGKILL backstop in case the child ignores SIGTERM. With the uWS
  // shutdown fix in src/backend/main.cpp, this should never fire for
  // first-party builds; keep it as defense-in-depth against a hung child.
  // Cancel the timer when the process exits cleanly — otherwise we'd
  // keep the event loop alive for 3 s after every stop and silently
  // SIGKILL a dead PID (ESRCH, swallowed below).
  const killTimer = setTimeout(() => {
    try { proc.kill("SIGKILL"); } catch {}  // proc may already be dead (ESRCH)
  }, 3000);
  proc.once("exit", () => clearTimeout(killTimer));
  return done;
}

let mainWindow = null;

function createWindow() {
  mainWindow = new BrowserWindow({
    width: 1400,
    height: 900,
    minWidth: 900,
    minHeight: 600,
    backgroundColor: "#09090B",
    title: "SignalScope",
    icon: path.join(__dirname, "..", "..", "assets", "icon.png"),
    webPreferences: {
      preload: path.join(__dirname, "preload.js"),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
    },
    frame: false,
    // E2E tests set SIGNALSCOPE_E2E=1 so the window stays off-screen
    // instead of blocking the developer's desktop on every run.
    show: process.env.SIGNALSCOPE_E2E !== "1",
  });

  const isDev = process.env.NODE_ENV === "development";

  // Resolve renderer path from app root (works in both dev and packaged)
  const appRoot = app.getAppPath();
  const htmlPath = path.join(appRoot, "dist", "renderer", "index.html");

  mainWindow.loadFile(htmlPath).catch((err) => {
    console.error("[main] Failed to load:", err.message);
    // Fallback: try loading from src directly
    const fallback = path.join(appRoot, "src", "renderer", "index.html");
    console.log("[main] Trying fallback:", fallback);
    if (fs.existsSync(fallback)) mainWindow.loadFile(fallback);
  });

  if (isDev) {
    mainWindow.webContents.openDevTools({ mode: "detach" });
  }

  // Pass backend connection info to renderer once ready
  mainWindow.webContents.on("did-finish-load", () => {
    mainWindow.webContents.send("backend-config", {
      wsUrl: `ws://localhost:${backendPort}`,
      port: backendPort,
    });
  });

  mainWindow.on("maximize", () => {
    mainWindow.webContents.send("window:state-change", { maximized: true });
  });
  mainWindow.on("unmaximize", () => {
    mainWindow.webContents.send("window:state-change", { maximized: false });
  });

  mainWindow.on("closed", () => {
    mainWindow = null;
  });
}

// GPU flags must be set before app.whenReady. WSL2's GPU driver is on
// Chromium's blocklist by default due to early driver bugs since fixed.
app.commandLine.appendSwitch("ignore-gpu-blocklist");
app.commandLine.appendSwitch("enable-webgl");
app.commandLine.appendSwitch("enable-gpu-rasterization");
app.commandLine.appendSwitch("enable-zero-copy");

app.whenReady().then(() => {
  ipcMain.handle("get-backend-config", async () => {
    // Block the renderer's reply until the backend confirms it's actually
    // listening on the port (or until the readiness Promise settles with a
    // fatal / timeout / exit reason). Without this await the renderer would
    // race the backend and connect to a not-yet-listening port, fall back
    // to simulation, and never re-try - see C5 in the 2026-05-13 review.
    const result = await backendReadyPromise;
    if (result.ready) {
      return {
        wsUrl: `ws://localhost:${result.port}`,
        port: result.port,
        running: true,
      };
    }
    return {
      wsUrl: `ws://localhost:${backendPort}`,
      port: backendPort,
      running: false,
      reason: result.reason,
    };
  });

  ipcMain.handle("restart-backend", async () => {
    await stopBackend();
    return { success: startBackend() };
  });

  ipcMain.handle("start-backend", async () => {
    if (backendProcess) return { success: true, already: true };
    return { success: startBackend() };
  });

  ipcMain.handle("stop-backend", async () => {
    await stopBackend();
    return { success: true };
  });

  ipcMain.handle("dialog:save-file", async (event, opts = {}) => {
    const win = BrowserWindow.fromWebContents(event.sender);
    const result = await dialog.showSaveDialog(win, {
      title: opts.title || "Save",
      defaultPath: opts.defaultPath,
      filters: opts.filters,
    });
    if (result.canceled) return null;
    return result.filePath;
  });

  ipcMain.handle("dialog:open-file", async (event) => {
    const win = BrowserWindow.fromWebContents(event.sender);
    const result = await dialog.showOpenDialog(win, {
      title: "Load Audio / IQ File",
      filters: [
        { name: "Signal Files", extensions: ["wav", "aiff", "aif", "f32", "raw", "sigmf-data"] },
        { name: "All Files", extensions: ["*"] },
      ],
      properties: ["openFile"],
    });
    if (result.canceled) return null;
    const filePath = result.filePaths[0];
    return resolveSignalFile(filePath);
  });

  ipcMain.handle("window:minimize", (event) => {
    const win = BrowserWindow.fromWebContents(event.sender);
    if (win) win.minimize();
  });

  ipcMain.handle("window:close", (event) => {
    const win = BrowserWindow.fromWebContents(event.sender);
    if (win) win.close();
  });

  ipcMain.handle("window:toggle-maximize", (event) => {
    const win = BrowserWindow.fromWebContents(event.sender);
    if (!win) return;
    // On Linux we use setBounds() rather than win.maximize(), so the native
    // isMaximized() bit never flips — track our own pseudo-maximized flag.
    // Also send window:state-change directly from the handler regardless of
    // platform, so the renderer titlebar updates even when the OS-level
    // maximize/unmaximize events don't fire (e.g. show:false in E2E tests).
    const wasMaximized = win.__pseudoMaximized === true || win.isMaximized();
    if (wasMaximized) {
      if (win.__prevBounds) {
        win.setBounds(win.__prevBounds);
        win.__prevBounds = null;
      } else if (win.isMaximized()) {
        win.unmaximize();
      }
      win.__pseudoMaximized = false;
      win.webContents.send("window:state-change", { maximized: false });
    } else {
      if (process.platform === "linux") {
        // On Linux, win.maximize() doesn't reliably cover the full screen
        // with frame:false + XWayland, so we set bounds manually.
        win.__prevBounds = win.getBounds();
        const { workArea } = screen.getDisplayMatching(win.getBounds());
        win.setBounds(workArea);
      } else {
        win.maximize();
      }
      win.__pseudoMaximized = true;
      win.webContents.send("window:state-change", { maximized: true });
      win.focus();
    }
  });

  // Always try to start the backend. If the binary is missing, startBackend()
  // returns false and the renderer falls back to simulation automatically via
  // the WebSocket reconnect loop (it will keep retrying every 2 s).
  const noBackend =
    process.argv.includes("--no-backend") ||
    process.env.SIGNALSCOPE_NO_BACKEND === "1";

  if (!noBackend) {
    startBackend();
  } else {
    console.log("[main] Backend disabled - running in simulation mode");
  }

  // Open the window immediately; the WebSocket reconnect loop handles the
  // race between renderer load and backend ready state.
  createWindow();

  app.on("activate", () => {
    if (BrowserWindow.getAllWindows().length === 0) {
      createWindow();
    }
  });
});

app.on("window-all-closed", async () => {
  await stopBackend();
  if (process.platform !== "darwin") {
    app.quit();
  }
});

app.on("before-quit", () => {
  // Best-effort synchronous SIGTERM. The async stopBackend can't complete
  // here because app.quit() is on the call stack. The will-quit handler
  // (added next) is the hard backstop that prevents orphan child processes.
  if (backendProcess) {
    try { backendProcess.kill("SIGTERM"); } catch {}  // may already be dead
  }
});

app.on("will-quit", () => {
  // Last resort: hard-kill the backend if SIGTERM didn't take effect
  // before Electron's main process tears down. Without this, an
  // unresponsive backend (e.g. older builds without the uWS shutdown
  // fix) survives as an orphan zombie holding port 8765.
  if (backendProcess) {
    try { backendProcess.kill("SIGKILL"); } catch {}  // may already be dead
  }
});

// Handle uncaught exceptions gracefully
process.on("uncaughtException", (err) => {
  console.error("Uncaught exception:", err);
  // Fire-and-forget: the process is already in a fatal state, we just
  // want to kick off the SIGTERM. The will-quit SIGKILL backstop will
  // catch the child even if this Promise never settles.
  stopBackend().catch(() => {});
});
