import { useState, useEffect } from "react";

export function TitleBar() {
  const [maximized, setMaximized] = useState(false);

  useEffect(() => {
    window.signalscope?.onWindowStateChange?.((state) => setMaximized(state.maximized));
  }, []);

  return (
    <div
      data-testid="titlebar-root"
      data-maximized={maximized ? "1" : "0"}
      className={`app-titlebar${maximized ? " app-titlebar--maximized" : ""}`}
    >
      <div className="app-titlebar-title" />
      <div className="app-titlebar-controls">
        <button
          data-testid="titlebar-minimize"
          className="titlebar-button"
          onClick={() => window.signalscope?.minimizeWindow()}
        >
          −
        </button>
        <button
          data-testid="titlebar-maximize"
          className="titlebar-button"
          onClick={() => window.signalscope?.toggleMaximizeWindow()}
          title={maximized ? "Restore" : "Maximize"}
        >
          {maximized ? "❐" : "□"}
        </button>
        <button
          data-testid="titlebar-close"
          className="titlebar-button titlebar-button-close"
          onClick={() => window.signalscope?.closeWindow()}
        >
          ×
        </button>
      </div>
    </div>
  );
}
