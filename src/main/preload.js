const { contextBridge, ipcRenderer } = require("electron");

contextBridge.exposeInMainWorld("signalscope", {
  getBackendConfig: () => ipcRenderer.invoke("get-backend-config"),
  restartBackend: () => ipcRenderer.invoke("restart-backend"),
  startBackend: () => ipcRenderer.invoke("start-backend"),
  stopBackend: () => ipcRenderer.invoke("stop-backend"),

  minimizeWindow: () => ipcRenderer.invoke("window:minimize"),
  closeWindow: () => ipcRenderer.invoke("window:close"),
  toggleMaximizeWindow: () => ipcRenderer.invoke("window:toggle-maximize"),

  onBackendConfig: (callback) => {
    ipcRenderer.on("backend-config", (_event, config) => callback(config));
  },
  onWindowStateChange: (callback) => {
    ipcRenderer.on("window:state-change", (_event, state) => callback(state));
  },
  openFile: () => ipcRenderer.invoke("dialog:open-file"),
  saveFile: (opts) => ipcRenderer.invoke("dialog:save-file", opts),

  platform: process.platform,
});
