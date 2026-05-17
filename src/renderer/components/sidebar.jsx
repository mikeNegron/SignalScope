import { T } from "../lib/tokens.js";
import { MeasureTab } from "./sidebar/measure-tab.jsx";
import { CursorsTab } from "./sidebar/cursors-tab.jsx";
import { InputTab } from "./sidebar/input-tab.jsx";
import { SettingsTab } from "./sidebar/settings-tab.jsx";
import { PeaksTab } from "./sidebar/peaks-tab.jsx";

const TABS = ["measure", "cursors", "peaks", "input", "settings"];

export function Sidebar({
  sidebarTab,
  onSidebarTab,
  sbW,
  // MeasureTab
  meas,
  backendClipping,
  backendConnected,
  backendFrameId,
  timeRef,
  srcLabel,
  // CursorsTab
  cursorF1,
  cursorF2,
  spectrumData,
  sampleRate,
  fftSize,
  onClearF1,
  onClearF2,
  onClearCursors,
  // InputTab
  source,
  onSource,
  onOpenFile,
  onSampleRate,
  onReconnect,
  connColor,
  replaySpeed,
  onReplaySpeed,
  listenPort,
  listenFormat,
  listenSampleRate,
  listenCenterFreq,
  listenExpectHeader,
  listenStatus,
  onListenPort,
  onListenFormat,
  onListenSampleRate,
  onListenCenterFreq,
  onListenExpectHeader,
  // SettingsTab
  brightness,
  contrast,
  scrollSpeed,
  onBrightness,
  onContrast,
  onScrollSpeed,
  waveformData,
  freqScale,
  onFreqScale,
  freqScaleDisabled,
  sourceComplex,
  tuneHz,
  decimation,
  onTune,
  onDecimation,
  suppressImage,
  onSuppressImage,
  weighting,
  onWeighting,
  overlap,
  onOverlap,
  taperCount,
  onTaperCount,
  historyCapacity,
  onHistoryCapacity,
  historyChunk,
  onHistoryChunk,
  // PeaksTab
  snapStats,
  centerFreq,
  spectrumTwoSided,
  peakMinLevel,
  onPeakMinLevel,
  onExport,
}) {
  return (
    <div
      data-testid="sidebar-root"
      style={{
        width: sbW,
        display: "flex",
        flexDirection: "column",
        flexShrink: 0,
        borderLeft: `1px solid ${T.border}`,
        background: T.bgPanel,
      }}
    >
      <div style={{ display: "flex", borderBottom: `1px solid ${T.border}` }}>
        {TABS.map((t) => (
          <button
            key={t}
            data-testid={`sidebar-tab-${t}`}
            onClick={() => onSidebarTab(t)}
            style={{
              flex: 1,
              padding: "6px 0",
              background: sidebarTab === t ? T.bgElev : "transparent",
              color: sidebarTab === t ? T.primary : T.textMuted,
              border: "none",
              borderBottom:
                sidebarTab === t
                  ? `2px solid ${T.primary}`
                  : "2px solid transparent",
              fontFamily: T.font,
              fontSize: 9,
              fontWeight: 600,
              textTransform: "uppercase",
              cursor: "pointer",
            }}
          >
            {t}
          </button>
        ))}
      </div>
      <div style={{ flex: 1, overflow: "auto", padding: "8px 10px" }}>
        {sidebarTab === "measure" && (
          <MeasureTab
            meas={meas}
            backendClipping={backendClipping}
            backendConnected={backendConnected}
            backendFrameId={backendFrameId}
            timeRef={timeRef}
            srcLabel={srcLabel}
          />
        )}
        {sidebarTab === "cursors" && (
          <CursorsTab
            cursorF1={cursorF1}
            cursorF2={cursorF2}
            spectrumData={spectrumData}
            sampleRate={sampleRate}
            fftSize={fftSize}
            onClearF1={onClearF1}
            onClearF2={onClearF2}
            onClearAll={onClearCursors}
          />
        )}
        {sidebarTab === "peaks" && (
          <PeaksTab
            peaks={snapStats?.peaks ?? []}
            noiseFloor={snapStats?.noiseFloor}
            fundamentalFreq={snapStats?.fundamentalFreq ?? 0}
            sampleRate={sampleRate}
            centerFreq={centerFreq}
            spectrumTwoSided={spectrumTwoSided}
            peakMinLevel={peakMinLevel}
            onPeakMinLevel={onPeakMinLevel}
          />
        )}
        {sidebarTab === "input" && (
          <InputTab
            backendConnected={backendConnected}
            sampleRate={sampleRate}
            onSampleRate={onSampleRate}
            source={source}
            onSource={onSource}
            onOpenFile={onOpenFile}
            onReconnect={onReconnect}
            connColor={connColor}
            replaySpeed={replaySpeed}
            onReplaySpeed={onReplaySpeed}
            listenPort={listenPort}
            listenFormat={listenFormat}
            listenSampleRate={listenSampleRate}
            listenCenterFreq={listenCenterFreq}
            listenExpectHeader={listenExpectHeader}
            listenStatus={listenStatus}
            onListenPort={onListenPort}
            onListenFormat={onListenFormat}
            onListenSampleRate={onListenSampleRate}
            onListenCenterFreq={onListenCenterFreq}
            onListenExpectHeader={onListenExpectHeader}
          />
        )}
        {sidebarTab === "settings" && (
          <SettingsTab
            brightness={brightness}
            contrast={contrast}
            scrollSpeed={scrollSpeed}
            onBrightness={onBrightness}
            onContrast={onContrast}
            onScrollSpeed={onScrollSpeed}
            waveformData={waveformData}
            spectrumData={spectrumData}
            sampleRate={sampleRate}
            freqScale={freqScale}
            onFreqScale={onFreqScale}
            freqScaleDisabled={freqScaleDisabled}
            tuneHz={tuneHz}
            decimation={decimation}
            onTune={onTune}
            onDecimation={onDecimation}
            suppressImage={suppressImage}
            onSuppressImage={onSuppressImage}
            sourceComplex={sourceComplex}
            weighting={weighting}
            onWeighting={onWeighting}
            overlap={overlap}
            onOverlap={onOverlap}
            taperCount={taperCount}
            onTaperCount={onTaperCount}
            historyCapacity={historyCapacity}
            onHistoryCapacity={onHistoryCapacity}
            historyChunk={historyChunk}
            onHistoryChunk={onHistoryChunk}
            onExport={onExport}
            backendConnected={backendConnected}
          />
        )}
      </div>
    </div>
  );
}
