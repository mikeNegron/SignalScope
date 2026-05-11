import { useState, useEffect } from "react";

export function TitleBar() {
  const [maximized, setMaximized] = useState(false);

  useEffect(() => {
    window.signalscope?.onWindowStateChange?.((state) => setMaximized(state.maximized));
  }, []);

  return (
    <div className={`app-titlebar${maximized ? " app-titlebar--maximized" : ""}`}>
      <div className="app-titlebar-title" />
      <div className="app-titlebar-controls">
        <button
          className="titlebar-button"
          onClick={() => window.signalscope?.minimizeWindow()}
        >
          −
        </button>
        <button
          className="titlebar-button"
          onClick={() => window.signalscope?.toggleMaximizeWindow()}
          title={maximized ? "Restore" : "Maximize"}
        >
          {maximized ? "❐" : "□"}
        </button>
        <button
          className="titlebar-button titlebar-button-close"
          onClick={() => window.signalscope?.closeWindow()}
        >
          ×
        </button>
      </div>
    </div>
  );
}
