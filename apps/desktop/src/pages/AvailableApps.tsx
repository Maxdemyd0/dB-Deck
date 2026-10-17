import { useState } from "react";
import { Icon } from "../components/Icon";
import { request } from "../engine/store";
import type { EngineState } from "../engine/types";

/**
 * Applications with Windows audio sessions that are not in the mixer yet.
 * Suggestions only: nothing is added until the user clicks.
 */
export function AvailableApps({ state, icons, searchRef }: { state: EngineState; icons: Record<string, string>; searchRef: React.RefObject<HTMLInputElement> }) {
  const [q, setQ] = useState("");
  const apps = state.availableApps.filter(
    (a) => !q || a.displayName.toLowerCase().includes(q.toLowerCase()) || a.exeName.includes(q.toLowerCase()),
  );
  return (
    <aside className="apps-panel" aria-label="Available apps">
      <div className="apps-head">
        <h3>
          Available Apps <span className="badge">{state.availableApps.length}</span>
        </h3>
        <input ref={searchRef} type="search" placeholder="Search apps  (Ctrl+F)" value={q} onChange={(e) => setQ(e.target.value)} />
      </div>
      <div className="apps-list">
        {apps.map((a) => (
          <div className="app-item" key={a.exeName}>
            <div className="strip-icon">{icons[a.iconKey] ? <img src={icons[a.iconKey]} alt="" /> : <Icon name="app" />}</div>
            <div className="info">
              <div className="nm" title={a.displayName}>{a.displayName}</div>
              <div className="meta" title={a.devices.join(", ")}>
                <span className={`dot ${a.state === "active" ? "ok" : ""}`} style={{ display: "inline-block", marginRight: 5, width: 6, height: 6 }} />
                {a.state === "active" ? "Playing" : "Idle"} · {a.devices[0] ?? ""}
              </div>
            </div>
            <button className="btn small icon" title={`Add ${a.displayName} to the mixer`} aria-label={`Add ${a.displayName}`} onClick={() => request("createChannel", { type: "app", exeName: a.exeName, name: a.displayName }).catch(() => {})}>
              <Icon name="plus" size={14} />
            </button>
          </div>
        ))}
        {state.availableApps.length === 0 && (
          <div className="faint small" style={{ padding: "16px 8px", lineHeight: 1.5 }}>
            Apps appear here when they open an audio stream in Windows. Start playing something and it will show up.
          </div>
        )}
        {state.availableApps.length > 0 && apps.length === 0 && <div className="faint small" style={{ padding: 8 }}>No matches.</div>}
      </div>
    </aside>
  );
}
