import { invoke } from "@tauri-apps/api/core";
import { useEffect, useState } from "react";
import { HotkeysEditor } from "../components/HotkeysEditor";
import { inTauri, request, toast } from "../engine/store";
import type { EngineState } from "../engine/types";

export function SettingsPage({ state, theme, setTheme }: { state: EngineState; theme: "dark" | "light"; setTheme: (t: "dark" | "light") => void }) {
  const [autostart, setAutostart] = useState<boolean | null>(null);
  useEffect(() => {
    if (inTauri) invoke<boolean>("get_autostart").then(setAutostart).catch(() => setAutostart(null));
  }, []);
  const s = state.settings;
  return (
    <div className="page">
      <h1 className="page-title">Settings</h1>
      <div className="card">
        <h3>General</h3>
        <div className="list">
          <label className="list-row">
            <span className="grow">Start with Windows<div className="faint small">Starts minimized to the tray so your routing is ready after sign-in.</div></span>
            <input
              type="checkbox"
              checked={!!autostart}
              disabled={autostart === null}
              onChange={(e) =>
                invoke<boolean>("set_autostart", { enabled: e.target.checked })
                  .then(setAutostart)
                  .catch((err) => toast("error", `Could not change startup setting: ${err}`))
              }
            />
          </label>
          <label className="list-row">
            <span className="grow">Appearance</span>
            <div className="seg">
              <button className={theme === "dark" ? "on" : ""} onClick={() => setTheme("dark")}>Dark</button>
              <button className={theme === "light" ? "on" : ""} onClick={() => setTheme("light")}>Light</button>
            </div>
          </label>
          <label className="list-row">
            <span className="grow">Show “System Sounds” in Available Apps</span>
            <input type="checkbox" checked={s.showSystemSounds} onChange={(e) => request("updateSettings", { showSystemSounds: e.target.checked }).catch(() => {})} />
          </label>
        </div>
      </div>
      <div className="card">
        <h3>Audio engine</h3>
        <div className="list">
          <div className="list-row">
            <span className="grow">
              Buffering
              <div className="faint small">Lower buffering reduces latency but needs a stable system. Changing it briefly restarts audio streams.</div>
            </span>
            <div className="seg">
              {(["low", "normal", "safe"] as const).map((m) => (
                <button key={m} className={s.bufferMode === m ? "on" : ""} onClick={() => request("updateSettings", { bufferMode: m }).catch(() => {})}>
                  {m[0].toUpperCase() + m.slice(1)}
                </button>
              ))}
            </div>
          </div>
          <div className="list-row">
            <span className="grow">Meter refresh rate</span>
            <select value={s.meterRateHz} onChange={(e) => request("updateSettings", { meterRateHz: Number(e.target.value) }).catch(() => {})}>
              {[15, 30, 60].map((r) => <option key={r} value={r}>{r} Hz</option>)}
            </select>
          </div>
          <div className="list-row">
            <span className="grow">Internal format</span>
            <span className="muted">{state.engine.sampleRate / 1000} kHz · 32-bit float · stereo</span>
          </div>
        </div>
      </div>
      <HotkeysEditor state={state} />
      <div className="card">
        <h3>Keyboard shortcuts</h3>
        <table className="tbl">
          <tbody>
            {[
              ["Ctrl+1 / 2 / 3", "Simple / Mixer / Advanced view"],
              ["Ctrl+M", "Mute all managed audio"],
              ["Ctrl+Z", "Undo last mixer change"],
              ["Ctrl+Y or Ctrl+Shift+Z", "Redo"],
              ["Ctrl+F", "Search Available Apps"],
              ["Ctrl+Shift+C", "Clear clip lights"],
              ["Wheel or arrow keys on a fader", "1 dB steps (Ctrl 0.1 dB, Shift 3 dB); Home = unity"],
              ["Double-click a fader", "Reset to unity"],
              ["Click the level under a fader", "Type an exact level (e.g. -6, +3.5, -inf)"],
              ["Right-click a strip", "Its menu"],
            ].map(([k, v]) => (
              <tr key={k}><td style={{ width: 220 }}><kbd>{k}</kbd></td><td className="muted">{v}</td></tr>
            ))}
          </tbody>
        </table>
      </div>
      <div className="card">
        <h3>About</h3>
        <p className="muted" style={{ margin: 0 }}>
          dB-Deck {state.engine.version} · protocol v{state.engine.protocol} · free and open source (MIT). No account, no telemetry.
        </p>
      </div>
    </div>
  );
}
