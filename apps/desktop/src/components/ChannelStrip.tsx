import { useState } from "react";
import { FADER_MAX, faderLabel } from "../engine/format";
import { request } from "../engine/store";
import type { Bus, Channel, ChannelRuntime, Device, Group } from "../engine/types";
import { Fader } from "./Fader";
import { Icon } from "./Icon";
import { Meter } from "./Meter";
import { MenuButton, MenuItem } from "./Overlay";

export type View = "simple" | "mixer" | "advanced";

const typeIcon: Record<string, string> = { app: "app", hardwareInput: "mic", endpointLoopback: "loop", testTone: "tone" };

export function statusInfo(c: Channel, rt?: ChannelRuntime): { text: string; cls: string } {
  const s = rt?.status ?? "offline";
  if (c.type === "app") {
    if (s === "active") return { text: "Playing", cls: "ok" };
    if (s === "idle") return { text: "Idle", cls: "" };
    return { text: "Not running", cls: "warn" };
  }
  if (s === "connected" || s === "generated") return { text: s === "generated" ? "Generated tone" : "Connected", cls: "ok" };
  if (s === "disconnected") return { text: "Disconnected", cls: "danger" };
  if (s === "error") return { text: "Error", cls: "danger" };
  return { text: s, cls: "" };
}

interface Props {
  channel: Channel;
  runtime?: ChannelRuntime;
  buses: Bus[];
  groups: Group[];
  devices: Device[];
  icon?: string;
  view: View;
  dragging: boolean;
  dropBefore: boolean;
  onDragStart: () => void;
  onDragEnd: () => void;
  onDragOverStrip: () => void;
  onDropStrip: () => void;
  onEffects: () => void;
  onMatchRules: () => void;
  onNewGroup: () => void;
}

export function ChannelStrip(p: Props) {
  const { channel: c, runtime: rt, view } = p;
  const [renaming, setRenaming] = useState(false);
  const [name, setName] = useState(c.name);
  const managed = rt?.capability === "managed";
  // Windows session volume stops at 100%; managed audio can boost to +20 dB.
  const canBoost = c.type !== "app" || c.appCapture;
  const status = statusInfo(c, rt);
  const offline = rt?.status === "offline" || rt?.status === "disconnected";
  const update = (args: Record<string, unknown>) => request("updateChannel", { id: c.id, ...args }).catch(() => {});

  const commitName = () => {
    setRenaming(false);
    if (name.trim() && name !== c.name) void update({ name: name.trim() });
    else setName(c.name);
  };

  const menu: MenuItem[] = [
    { label: "Rename", onClick: () => (setName(c.name), setRenaming(true)) },
    { heading: true, label: "Group" },
    ...p.groups.map((g) => ({ label: g.name, checked: c.groupId === g.id, onClick: () => update({ groupId: g.id }) })),
    { label: "New group…", onClick: p.onNewGroup },
    ...(c.groupId ? [{ label: "Remove from group", onClick: () => update({ groupId: "" }) }] : []),
    { separator: true, label: "" },
  ];
  if (c.type === "app") {
    menu.push({
      label: c.appCapture ? "Stop capturing app audio" : "Capture app audio (managed routing)",
      checked: c.appCapture,
      onClick: () => update({ appCapture: !c.appCapture }),
    });
    menu.push({ label: "Matching rules…", onClick: p.onMatchRules });
    // Where the app itself plays (Windows per-app output preference).
    const current = rt?.appOutputDeviceId;
    const setOutput = (deviceId: string) =>
      request("setAppOutput", { channelId: c.id, deviceId }).catch(() => {});
    menu.push({ separator: true, label: "" });
    if (current === null || current === undefined) {
      menu.push({ heading: true, label: "Output device (start the app to change)" });
    } else {
      menu.push({ heading: true, label: "App output device" });
      menu.push({ label: "Windows default", checked: current === "", onClick: () => setOutput("") });
      for (const d of p.devices.filter((x) => x.flow === "render" && x.state === "active"))
        menu.push({ label: d.name, checked: current === d.id, onClick: () => setOutput(d.id) });
    }
  }
  menu.push({ label: managed ? "Effects…" : "Effects… (needs managed audio)", disabled: !managed, onClick: p.onEffects });
  menu.push({ separator: true, label: "" });
  menu.push({ label: "Remove channel", danger: true, onClick: () => request("deleteChannel", { id: c.id }).catch(() => {}) });

  const sessionHint =
    c.type === "app" && !managed
      ? "Controls this app's Windows session volume. dB-Deck does not receive its audio; it keeps playing on its own Windows output."
      : undefined;

  return (
    <div
      className={`strip${offline ? " offline" : ""}${p.dragging ? " dragging" : ""}${p.dropBefore ? " drop-before" : ""}`}
      onDragOver={(e) => {
        e.preventDefault();
        p.onDragOverStrip();
      }}
      onDrop={(e) => {
        e.preventDefault();
        p.onDropStrip();
      }}
      data-channel={c.id}
    >
      <div
        className="strip-head"
        draggable
        onDragStart={(e) => {
          e.dataTransfer.effectAllowed = "move";
          e.dataTransfer.setData("text/plain", c.id);
          p.onDragStart();
        }}
        onDragEnd={p.onDragEnd}
        title="Drag to reorder"
      >
        <div className="strip-icon">{p.icon ? <img src={p.icon} alt="" /> : <Icon name={typeIcon[c.type]} />}</div>
        <div className="strip-name" onDoubleClick={() => (setName(c.name), setRenaming(true))} title={c.name}>
          {renaming ? (
            <input
              autoFocus
              value={name}
              onChange={(e) => setName(e.target.value)}
              onBlur={commitName}
              onKeyDown={(e) => {
                if (e.key === "Enter") commitName();
                if (e.key === "Escape") (setRenaming(false), setName(c.name));
              }}
            />
          ) : (
            c.name
          )}
        </div>
        <MenuButton items={menu} label={`${c.name} options`} />
      </div>

      <div className="strip-sub" title={rt?.error || status.text}>
        <span className={`dot ${status.cls}`} />
        <span className="ellipsis grow">{status.text}</span>
        {view !== "simple" && (
          <span className={`badge ${managed ? "accent" : ""}`} title={sessionHint ?? "Audio flows through the dB-Deck engine"}>
            {managed ? "Managed" : c.type === "app" ? "Windows" : "—"}
          </span>
        )}
      </div>

      <div className="strip-body">
        <Meter kind="channels" id={c.id} />
        <Fader
          value={c.volume}
          label={`${c.name} volume`}
          muted={c.mute}
          unity={canBoost ? 1 : undefined}
          max={canBoost ? FADER_MAX : 1}
          title={sessionHint}
          onChange={(v) => void update({ volume: Math.round(v * 1000) / 1000 })}
        />
      </div>
      <div className="strip-value" title={canBoost ? "Up to +20 dB" : "Windows app volume (max 100%). Enable app capture to boost."}>{faderLabel(c.volume, c.type !== "app")}</div>

      {view === "advanced" && managed && (
        <div className="col" style={{ gap: 4 }}>
          <div className="knob-row">
            <span>Trim</span>
            <input className="hslider" type="range" min={-24} max={24} step={0.5} value={c.gainDb} onChange={(e) => update({ gainDb: Number(e.target.value) })} />
            <span className="mono">{c.gainDb > 0 ? "+" : ""}{c.gainDb.toFixed(1)}</span>
          </div>
          <div className="knob-row">
            <span>Balance</span>
            <input className="hslider" type="range" min={-1} max={1} step={0.05} value={c.balance} onChange={(e) => update({ balance: Number(e.target.value) })} onDoubleClick={() => update({ balance: 0 })} />
            <span className="mono">{c.balance === 0 ? "C" : c.balance < 0 ? `L${Math.round(-c.balance * 100)}` : `R${Math.round(c.balance * 100)}`}</span>
          </div>
        </div>
      )}

      <div className="strip-foot">
        <button className={`btn small mute${c.mute ? " on" : ""}`} onClick={() => update({ mute: !c.mute })} aria-pressed={c.mute} title="Mute">
          M
        </button>
        {view !== "simple" && managed && (
          <button className={`btn small toggle${c.effects.some((e) => e.enabled) ? " on" : ""}`} onClick={p.onEffects} title="Effects">
            FX
          </button>
        )}
      </div>

      {view !== "simple" && <Sends channel={c} runtime={rt} buses={p.buses} detailed={view === "advanced"} />}
    </div>
  );
}

function Sends({ channel: c, runtime: rt, buses, detailed }: { channel: Channel; runtime?: ChannelRuntime; buses: Bus[]; detailed: boolean }) {
  return (
    <div className="sends">
      {buses.map((b) => {
        const s = c.sends.find((x) => x.busId === b.id);
        const st = rt?.sends[b.id];
        const on = !!s?.enabled;
        const cls = !on ? "" : st?.state === "blocked" ? " blocked" : st?.state === "session-only" ? " session" : " on";
        const tip =
          st?.reason ||
          (on ? `Sends to ${b.name}${s?.preFader ? " (pre-fader)" : ""}` : `Not routed to ${b.name}. Click to route.`);
        return (
          <div className="send" key={b.id}>
            <button
              className={`send-toggle${cls}`}
              title={tip}
              aria-pressed={on}
              onClick={() => request("setSend", { channelId: c.id, busId: b.id, enabled: !on }).catch(() => {})}
            >
              {st?.state === "blocked" && on ? <Icon name="warn" size={11} /> : null}
              <span className="nm">{b.name}</span>
            </button>
            <span className="pct">{on ? `${Math.round((s?.gain ?? 1) * 100)}%` : "off"}</span>
            {detailed && on && (
              <input
                className="hslider"
                style={{ gridColumn: "1 / -1" }}
                type="range"
                min={0}
                max={1.5}
                step={0.01}
                value={s?.gain ?? 1}
                onChange={(e) => request("setSend", { channelId: c.id, busId: b.id, gain: Number(e.target.value) }, true).catch(() => {})}
                title={`${b.name} send level`}
              />
            )}
          </div>
        );
      })}
    </div>
  );
}
