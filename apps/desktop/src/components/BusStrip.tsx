import { CSSProperties, useRef, useState } from "react";
import { FADER_MAX } from "../engine/format";
import { deleteWithUndo, request } from "../engine/store";
import type { Bus, BusRuntime, Device } from "../engine/types";
import { Fader } from "./Fader";
import { Icon } from "./Icon";
import { Meter } from "./Meter";
import { colorMenuItems, MenuButton } from "./Overlay";
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

export interface BusDrag {
  dragging: boolean;
  dropBefore: boolean;
  onDragStart: () => void;
  onDragEnd: () => void;
  onDragOverStrip: () => void;
  onDropStrip: () => void;
}

export function BusStrip({ bus, runtime, devices, onEffects, drag }: { bus: Bus; runtime?: BusRuntime; devices: Device[]; onEffects: () => void; drag?: BusDrag }) {
  const st = busStatus(runtime);
  const root = useRef<HTMLDivElement>(null);
  const [renaming, setRenaming] = useState(false);
  const [name, setName] = useState(bus.name);
  const update = (args: Record<string, unknown>) => request("updateBus", { id: bus.id, ...args }).catch(() => {});
  const renders = devices.filter((d) => d.flow === "render" && d.state === "active");
  const setDevice = (deviceId: string) =>
    bus.isMain ? request("setMainOutput", { deviceId }).catch(() => {}) : update({ deviceId });
  const startRename = () => (setName(bus.name), setRenaming(true));
  const commitName = () => {
    setRenaming(false);
    if (name.trim() && name !== bus.name) void update({ name: name.trim() });
    else setName(bus.name);
  };
  // Main stays first, so it is neither dragged nor a drop target.
  const movable = !bus.isMain && !!drag;
  return (
    <div
      ref={root}
      className={`strip bus${bus.isMain ? " main" : ""}${drag?.dragging ? " dragging" : ""}${drag?.dropBefore ? " drop-before" : ""}${bus.color ? " colored" : ""}`}
      style={bus.color ? ({ "--strip-color": bus.color } as CSSProperties) : undefined}
      onDragOver={movable ? (e) => (e.preventDefault(), drag!.onDragOverStrip()) : undefined}
      onDrop={movable ? (e) => (e.preventDefault(), drag!.onDropStrip()) : undefined}
    >
      <div
        className="strip-head"
        style={movable ? undefined : { cursor: "default" }}
        draggable={movable}
        onDragStart={
          movable
            ? (e) => {
                e.dataTransfer.effectAllowed = "move";
                e.dataTransfer.setData("text/plain", bus.id);
                drag!.onDragStart();
              }
            : undefined
        }
        onDragEnd={movable ? drag!.onDragEnd : undefined}
        title={movable ? "Drag to reorder" : undefined}
      >
        <div className="strip-icon">
          <Icon name={bus.isMain ? "output" : "bus"} />
        </div>
        <div className="strip-name" title={bus.name} onDoubleClick={startRename}>
          {renaming ? (
            <input
              autoFocus
              value={name}
              onChange={(e) => setName(e.target.value)}
              onBlur={commitName}
              onKeyDown={(e) => {
                if (e.key === "Enter") commitName();
                if (e.key === "Escape") (setRenaming(false), setName(bus.name));
              }}
            />
          ) : (
            bus.name
          )}
        </div>
        <MenuButton
          label={`${bus.name} options`}
          contextTarget={root}
          items={[
            { label: "Rename", onClick: startRename },
            { heading: true, label: "Output device" },
            { label: "None", checked: !bus.deviceId, onClick: () => setDevice("") },
            ...renders.map((d) => ({ label: d.name, checked: bus.deviceId === d.id, onClick: () => setDevice(d.id) })),
            { separator: true, label: "" },
            { label: "Effects…", onClick: onEffects },
            { separator: true, label: "" },
            ...colorMenuItems(bus.color, (color) => void update({ color })),
            ...(bus.isMain ? [] : [{ separator: true, label: "" }, { label: "Delete bus", danger: true, onClick: () => void deleteWithUndo("deleteBus", { id: bus.id }, `bus "${bus.name}"`) }]),
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
