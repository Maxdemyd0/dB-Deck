import { FADER_MAX } from "../engine/format";
import { request } from "../engine/store";
import type { Bus, BusRuntime, Device } from "../engine/types";
import { Fader } from "./Fader";
import { Icon } from "./Icon";
import { Meter } from "./Meter";
import { MenuButton } from "./Overlay";
import { StripValue } from "./StripValue";

export function busStatus(rt?: BusRuntime): { text: string; cls: string } {
  switch (rt?.status) {
    case "connected": return { text: rt.activeDeviceName || "Connected", cls: "ok" };
    case "fallback": return { text: `Fallback: ${rt.activeDeviceName}`, cls: "warn" };
    case "disconnected": return { text: `${rt.activeDeviceName || "Output"} disconnected`, cls: "danger" };
    case "error": return { text: rt.error || "Output error", cls: "danger" };
    default: return { text: "No output device", cls: "" };
  }
}

export function BusStrip({ bus, runtime, devices, onEffects }: { bus: Bus; runtime?: BusRuntime; devices: Device[]; onEffects: () => void }) {
  const st = busStatus(runtime);
  const update = (args: Record<string, unknown>) => request("updateBus", { id: bus.id, ...args }).catch(() => {});
  const renders = devices.filter((d) => d.flow === "render" && d.state === "active");
  const setDevice = (deviceId: string) =>
    bus.isMain ? request("setMainOutput", { deviceId }).catch(() => {}) : update({ deviceId });
  return (
    <div className={`strip bus${bus.isMain ? " main" : ""}`}>
      <div className="strip-head" style={{ cursor: "default" }}>
        <div className="strip-icon">
          <Icon name={bus.isMain ? "output" : "bus"} />
        </div>
        <div className="strip-name" title={bus.name}>{bus.name}</div>
        <MenuButton
          label={`${bus.name} options`}
          items={[
            { heading: true, label: "Output device" },
            { label: "None", checked: !bus.deviceId, onClick: () => setDevice("") },
            ...renders.map((d) => ({ label: d.name, checked: bus.deviceId === d.id, onClick: () => setDevice(d.id) })),
            { separator: true, label: "" },
            { label: "Effects…", onClick: onEffects },
            ...(bus.isMain ? [] : [{ separator: true, label: "" }, { label: "Delete bus", danger: true, onClick: () => request("deleteBus", { id: bus.id }).catch(() => {}) }]),
          ]}
        />
      </div>
      <div className="strip-sub" title={runtime?.error || st.text}>
        <span className={`dot ${st.cls}`} />
        <span className="ellipsis grow">{st.text}</span>
      </div>
      <div className="strip-body">
        <Meter kind="buses" id={bus.id} />
        <Fader value={bus.volume} unity={1} max={FADER_MAX} label={`${bus.name} level`} muted={bus.mute} onChange={(v) => void update({ volume: Math.round(v * 1000) / 1000 })} />
      </div>
      <StripValue value={bus.volume} max={FADER_MAX} scale="db" title="Up to +20 dB" onCommit={(v) => void update({ volume: Math.round(v * 1000) / 1000 })} />
      <div className="strip-foot">
        <button className={`btn small mute${bus.mute ? " on" : ""}`} onClick={() => update({ mute: !bus.mute })} aria-pressed={bus.mute}>M</button>
        <button className={`btn small toggle${bus.effects.some((e) => e.enabled) ? " on" : ""}`} onClick={onEffects}>FX</button>
      </div>
    </div>
  );
}
