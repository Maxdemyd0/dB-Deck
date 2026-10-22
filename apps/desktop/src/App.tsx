import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import { useEffect, useRef, useState } from "react";
import type { View } from "./components/ChannelStrip";
import { Icon } from "./components/Icon";
import { CLEAR_CLIPS_EVENT } from "./components/Meter";
import { Modal, Toasts } from "./components/Overlay";
import { WindowsVolume } from "./components/WindowsVolume";
import { busStatus } from "./components/BusStrip";
import { FADER_MAX, faderLabel } from "./engine/format";
import { inTauri, request, startEngineStore, useEngine } from "./engine/store";
import type { EngineState } from "./engine/types";
import { InputsPage, OutputsPage, RoutingPage, VirtualDevicesPage } from "./pages/DevicePages";
import { DiagnosticsPage } from "./pages/DiagnosticsPage";
import { MixerPage } from "./pages/MixerPage";
import { ProfilesPage } from "./pages/ProfilesPage";
import { SettingsPage } from "./pages/SettingsPage";
import logo from "../app-icon.svg";

type Page = "mixer" | "inputs" | "outputs" | "virtual" | "profiles" | "settings" | "routing" | "diagnostics";

function readPref<T extends string>(key: string, fallback: T): T {
  try {
    return (localStorage.getItem(key) as T) || fallback;
  } catch {
    return fallback;
  }
}
function writePref(key: string, v: string) {
  try {
    localStorage.setItem(key, v);
  } catch {
    /* storage unavailable */
  }
}

export function App() {
  const { state, connected, message, icons } = useEngine();
  const [page, setPage] = useState<Page>("mixer");
  const [view, setViewState] = useState<View>(() => readPref<View>("dbdeck.view", "simple"));
  const [theme, setThemeState] = useState<"dark" | "light">(() => readPref("dbdeck.theme", "dark"));
  const [confirmExit, setConfirmExit] = useState(false);
  const [collapsed, setCollapsed] = useState(false);
  const searchRef = useRef<HTMLInputElement>(null);

  useEffect(() => startEngineStore(), []);
  useEffect(() => {
    document.documentElement.dataset.theme = theme;
    writePref("dbdeck.theme", theme);
  }, [theme]);
  const setView = (v: View) => {
    setViewState(v);
    writePref("dbdeck.view", v);
    if (state) void request("setLayout", { layout: { ...state.profile.layout, view: v } }, true).catch(() => {});
  };

  useEffect(() => {
    if (!inTauri) return;
    const u1 = listen<string>("navigate", (e) => setPage(e.payload as Page));
    const u2 = listen("confirm-exit", () => setConfirmExit(true));
    return () => {
      void u1.then((f) => f());
      void u2.then((f) => f());
    };
  }, []);

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      const typing = (e.target as HTMLElement)?.tagName === "INPUT" && (e.target as HTMLInputElement).type !== "range";
      if (!e.ctrlKey || e.altKey) return;
      const k = e.key.toLowerCase();
      if (k === "c" && e.shiftKey) {
        window.dispatchEvent(new Event(CLEAR_CLIPS_EVENT));
        e.preventDefault();
        return;
      }
      if (k === "1" || k === "2" || k === "3") {
        setView((["simple", "mixer", "advanced"] as View[])[Number(k) - 1]);
        e.preventDefault();
      } else if (k === "m" && state) {
        void request("setMasterMute", { mute: !state.engine.masterMute }).catch(() => {});
        e.preventDefault();
      } else if (k === "z" && !typing) {
        void request("undo").catch(() => {});
        e.preventDefault();
      } else if (k === "f") {
        setPage("mixer");
        setTimeout(() => searchRef.current?.focus(), 0);
        e.preventDefault();
      }
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  });

  const nav: { id: Page; label: string; icon: string; advanced?: boolean }[] = [
    { id: "mixer", label: "Mixer", icon: "mixer" },
    { id: "inputs", label: "Inputs", icon: "input" },
    { id: "outputs", label: "Outputs", icon: "output" },
    { id: "virtual", label: "Virtual Devices", icon: "virtual" },
    { id: "profiles", label: "Profiles", icon: "profiles" },
    { id: "routing", label: "Routing", icon: "link", advanced: true },
    { id: "diagnostics", label: "Diagnostics", icon: "diag", advanced: true },
  ];

  return (
    <div className={`app${collapsed ? " sidebar-collapsed" : ""}`}>
      <Toolbar state={state} connected={connected} view={view} setView={setView} />
      <nav className="sidebar" aria-label="Sections">
        {nav
          .filter((n) => !n.advanced || view === "advanced")
          .map((n) => (
            <button key={n.id} className={`nav-item${page === n.id ? " active" : ""}`} onClick={() => setPage(n.id)} title={n.label}>
              <Icon name={n.icon} />
              <span>{n.label}</span>
              {n.id === "mixer" && state && state.availableApps.length > 0 && <span className="badge">{state.availableApps.length}</span>}
            </button>
          ))}
        <div className="sidebar-foot">
          <button className={`nav-item${page === "settings" ? " active" : ""}`} onClick={() => setPage("settings")} title="Settings">
            <Icon name="settings" />
            <span>Settings</span>
          </button>
          <button className="nav-item" onClick={() => setCollapsed(!collapsed)} title={collapsed ? "Expand sidebar" : "Collapse sidebar"}>
            <Icon name="sidebar" />
            <span>Collapse</span>
          </button>
        </div>
      </nav>
      <main className="main">
        {!connected && (
          <div className="banner">
            <Icon name="warn" />
            <span className="grow">{message || "Audio engine disconnected — reconnecting…"}</span>
            {state && <span className="faint small">Showing the last known state; controls resume when the engine is back.</span>}
          </div>
        )}
        {state && state.engine.warnings.length > 0 && (
          <div className="banner warn">
            <Icon name="warn" />
            <span className="grow">{state.engine.warnings.join(" ")}</span>
          </div>
        )}
        {!state ? (
          <div className="empty">
            <div className="art"><Icon name="power" size={30} /></div>
            <h2>Connecting to the dB-Deck audio engine</h2>
            <p>{message}</p>
          </div>
        ) : (
          <PageView page={page} state={state} view={view} icons={icons} searchRef={searchRef} theme={theme} setTheme={setThemeState} />
        )}
      </main>
      <Toasts />
      {confirmExit && <ExitDialog state={state} onClose={() => setConfirmExit(false)} />}
    </div>
  );
}

function PageView(p: { page: Page; state: EngineState; view: View; icons: Record<string, string>; searchRef: React.RefObject<HTMLInputElement>; theme: "dark" | "light"; setTheme: (t: "dark" | "light") => void }) {
  switch (p.page) {
    case "mixer": return <MixerPage state={p.state} view={p.view} icons={p.icons} appsOpen={false} searchRef={p.searchRef} />;
    case "inputs": return <InputsPage state={p.state} />;
    case "outputs": return <OutputsPage state={p.state} />;
    case "virtual": return <VirtualDevicesPage state={p.state} />;
    case "profiles": return <ProfilesPage state={p.state} />;
    case "routing": return <RoutingPage state={p.state} />;
    case "diagnostics": return <DiagnosticsPage state={p.state} />;
    case "settings": return <SettingsPage state={p.state} theme={p.theme} setTheme={p.setTheme} />;
  }
}

function Toolbar({ state, connected, view, setView }: { state: EngineState | null; connected: boolean; view: View; setView: (v: View) => void }) {
  const main = state?.profile.buses.find((b) => b.isMain);
  const mainRt = main ? state?.runtime.buses[main.id] : undefined;
  const st = busStatus(mainRt);
  const renders = state?.devices.filter((d) => d.flow === "render" && d.state === "active") ?? [];
  return (
    <header className="toolbar">
      <div className="brand"><img src={logo} alt="" /> dB-Deck</div>
      <div className="seg" role="tablist" aria-label="View">
        {(["simple", "mixer", "advanced"] as View[]).map((v) => (
          <button key={v} className={view === v ? "on" : ""} onClick={() => setView(v)} role="tab" aria-selected={view === v}>
            {v[0].toUpperCase() + v.slice(1)}
          </button>
        ))}
      </div>
      {state && (
        <>
          <div className="tb-sep" />
          <div className="tb-group" title="Active profile">
            <select value={state.activeProfileId} onChange={(e) => request("loadProfile", { id: e.target.value }).catch(() => {})} aria-label="Profile" style={{ maxWidth: 170 }}>
              {state.profiles.map((p) => <option key={p.id} value={p.id}>{p.name}</option>)}
            </select>
          </div>
          <div className="tb-group" title={`Main output — ${st.text}`}>
            <span className={`dot ${st.cls}`} />
            <select value={main?.deviceId ?? ""} onChange={(e) => request("setMainOutput", { deviceId: e.target.value }).catch(() => {})} aria-label="Main output device" style={{ maxWidth: 210 }}>
              <option value="">Main: no output</option>
              {main?.deviceId && !renders.some((d) => d.id === main.deviceId) && <option value={main.deviceId}>{main.deviceName} (disconnected)</option>}
              {renders.map((d) => <option key={d.id} value={d.id}>{d.name}</option>)}
            </select>
          </div>
          {main && (
            <div className="vol-widget" title="dB-Deck Main bus level (managed audio)">
              <span className="tb-label">Main</span>
              <input className="hslider" type="range" min={0} max={FADER_MAX} step={0.01} value={main.volume} aria-label="Main bus level (up to +20 dB)" onDoubleClick={() => request("updateBus", { id: main.id, volume: 1 }).catch(() => {})} onChange={(e) => request("updateBus", { id: main.id, volume: Number(e.target.value) }, true).catch(() => {})} />
              <span className="pct" style={{ width: 54 }}>{faderLabel(main.volume, true)}</span>
            </div>
          )}
        </>
      )}
      <div className="spacer" />
      {state && (state.engine.solo?.length ?? 0) > 0 && (
        <button
          className="btn small solo on"
          onClick={() => request("setSolo", { clear: true }).catch(() => {})}
          title="Channels are soloed on Main. Click to clear solo."
        >
          Solo ({state.engine.solo!.length}) ✕
        </button>
      )}
      {state && <WindowsVolume ep={state.windowsVolume.default} target="default" compact={view === "simple"} />}
      {state && (
        <button
          className={`btn small mute${state.engine.masterMute ? " on" : ""}`}
          onClick={() => request("setMasterMute", { mute: !state.engine.masterMute }).catch(() => {})}
          title="Mute all managed audio (Ctrl+M)"
          aria-pressed={state.engine.masterMute}
        >
          <Icon name="mute" size={14} /> {state.engine.masterMute ? "Muted" : "Mute all"}
        </button>
      )}
      {state && (
        <button className="btn ghost icon small" disabled={state.engine.undoDepth === 0} onClick={() => request("undo").catch(() => {})} title="Undo (Ctrl+Z)" aria-label="Undo">
          <Icon name="undo" />
        </button>
      )}
      <div className="health" title={connected ? `Engine ${state?.engine.version ?? ""} · clock: ${state?.engine.clock}` : "Engine disconnected"}>
        <span className={`dot ${!connected ? "danger" : state?.engine.healthy ? "ok" : "warn"}`} />
        {connected ? (state?.engine.healthy ? "Engine OK" : "Check outputs") : "Offline"}
      </div>
    </header>
  );
}

function ExitDialog({ state, onClose }: { state: EngineState | null; onClose: () => void }) {
  const managed = state ? state.profile.channels.filter((c) => state.runtime.channels[c.id]?.capability === "managed") : [];
  const outputs = state ? state.profile.buses.filter((b) => ["connected", "fallback"].includes(state.runtime.buses[b.id]?.status)) : [];
  const exit = (stopEngine: boolean) => invoke("exit_app", { stopEngine }).catch(() => {});
  return (
    <Modal
      title="Exit dB-Deck?"
      onClose={onClose}
      actions={
        <>
          <button className="btn" onClick={onClose}>Cancel</button>
          <button className="btn danger solid" onClick={() => exit(true)}>Exit and stop audio</button>
        </>
      }
    >
      {managed.length > 0 || outputs.length > 0 ? (
        <div className="notice warn" style={{ marginTop: 8 }}>
          <Icon name="warn" />
          <div>
            Exiting stops the dB-Deck audio engine. These will stop working:
            <ul style={{ margin: "6px 0 0", paddingLeft: 18 }}>
              {managed.map((c) => <li key={c.id}>{c.name} (routed through dB-Deck)</li>)}
              {outputs.map((b) => <li key={b.id}>{b.name} bus → {state!.runtime.buses[b.id].activeDeviceName}</li>)}
            </ul>
            Application volume levels you set stay as they are in Windows.
          </div>
        </div>
      ) : (
        <p className="muted">No managed audio is playing. App volume levels stay as they are in Windows.</p>
      )}
      <p className="faint small">To keep audio running, close the window instead — dB-Deck stays in the system tray.</p>
    </Modal>
  );
}
