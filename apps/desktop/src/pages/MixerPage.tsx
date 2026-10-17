import { useMemo, useState } from "react";
import { BusStrip } from "../components/BusStrip";
import { ChannelStrip, View } from "../components/ChannelStrip";
import { EffectsEditor } from "../components/EffectsEditor";
import { Fader } from "../components/Fader";
import { Icon } from "../components/Icon";
import { MenuButton, Modal, Prompt } from "../components/Overlay";
import { request } from "../engine/store";
import type { Channel, EngineState } from "../engine/types";
import { AvailableApps } from "./AvailableApps";

export function MixerPage({ state, view, icons, appsOpen, searchRef }: { state: EngineState; view: View; icons: Record<string, string>; appsOpen: boolean; searchRef: React.RefObject<HTMLInputElement> }) {
  const p = state.profile;
  const [dragId, setDragId] = useState<string | null>(null);
  const [dropTarget, setDropTarget] = useState<string | null>(null); // channel id or "group:<id>" or "end"
  const [fx, setFx] = useState<{ target: "channel" | "bus"; id: string } | null>(null);
  const [rules, setRules] = useState<string | null>(null);
  const [newGroupFor, setNewGroupFor] = useState<string | null | undefined>(undefined);
  const [addOpen, setAddOpen] = useState(false);
  const [renaming, setRenaming] = useState<{ id: string; name: string } | null>(null);

  const visibleBuses = p.buses;
  const ungrouped = p.channels.filter((c) => !c.groupId || !p.groups.some((g) => g.id === c.groupId));

  const drop = (beforeId: string | null, groupId: string | null) => {
    if (!dragId) return;
    const ids = p.channels.map((c) => c.id).filter((id) => id !== dragId);
    const idx = beforeId ? ids.indexOf(beforeId) : ids.length;
    ids.splice(idx < 0 ? ids.length : idx, 0, dragId);
    const dragged = p.channels.find((c) => c.id === dragId);
    void (async () => {
      try {
        if (dragged && groupId !== null && dragged.groupId !== groupId) await request("updateChannel", { id: dragId, groupId });
        await request("reorderChannels", { ids });
      } catch {
        /* toast shown */
      }
    })();
    setDragId(null);
    setDropTarget(null);
  };

  const strip = (c: Channel) => {
    const rt = state.runtime.channels[c.id];
    return (
      <ChannelStrip
        key={c.id}
        channel={c}
        runtime={rt}
        buses={visibleBuses}
        groups={p.groups}
        icon={rt?.iconKey ? icons[rt.iconKey] : undefined}
        view={view}
        dragging={dragId === c.id}
        dropBefore={dropTarget === c.id && dragId !== c.id}
        onDragStart={() => setDragId(c.id)}
        onDragEnd={() => (setDragId(null), setDropTarget(null))}
        onDragOverStrip={() => dragId && setDropTarget(c.id)}
        onDropStrip={() => drop(c.id, c.groupId || "")}
        onEffects={() => setFx({ target: "channel", id: c.id })}
        onMatchRules={() => setRules(c.id)}
        onNewGroup={() => setNewGroupFor(c.id)}
      />
    );
  };

  const fxEntity = fx ? (fx.target === "channel" ? p.channels.find((c) => c.id === fx.id) : p.buses.find((b) => b.id === fx.id)) : undefined;
  const empty = p.channels.length === 0;

  return (
    <div className={`mixer-layout${appsOpen ? " apps-open" : ""}`}>
      <div className={`workspace view-${view}`}>
        <div className="workspace-head">
          <h1 className="page-title grow" style={{ margin: 0 }}>Mixer</h1>
          {p.scenes.length > 0 && (
            <MenuButton
              className="btn small"
              icon="scene"
              label="Apply scene"
              items={[{ heading: true, label: "Apply scene" }, ...p.scenes.map((s) => ({ label: s.name, onClick: () => request("applyScene", { id: s.id }).catch(() => {}) }))]}
            />
          )}
          <button className="btn small" onClick={() => setNewGroupFor(null)}>
            <Icon name="group" size={14} /> New group
          </button>
          <button className="btn small primary" onClick={() => setAddOpen(true)}>
            <Icon name="plus" size={14} /> Add channel
          </button>
        </div>

        {empty ? (
          <div className="empty">
            <div className="art"><Icon name="mixer" size={30} /></div>
            <h2>Your mixer is empty</h2>
            <p>
              Pick an app from <strong>Available Apps</strong> to control its volume, or add a microphone or other input with
              <strong> Add channel</strong>. New channels go to the Main bus.
            </p>
            <button className="btn primary" onClick={() => setAddOpen(true)}><Icon name="plus" size={14} /> Add channel</button>
          </div>
        ) : (
          <>
            <div className="strips">
              {p.groups.map((g) => {
                const members = p.channels.filter((c) => c.groupId === g.id);
                return (
                  <div
                    key={g.id}
                    className={`group-block${dropTarget === "group:" + g.id ? " drop" : ""}`}
                    onDragOver={(e) => {
                      e.preventDefault();
                      if (dragId && (e.target as HTMLElement).closest(".strip") === null) setDropTarget("group:" + g.id);
                    }}
                    onDrop={(e) => {
                      e.preventDefault();
                      if ((e.target as HTMLElement).closest(".strip") === null) drop(null, g.id);
                    }}
                  >
                    <div className="group-head">
                      <button className="btn ghost icon small" onClick={() => request("updateGroup", { id: g.id, collapsed: !g.collapsed }).catch(() => {})} aria-label={g.collapsed ? "Expand group" : "Collapse group"}>
                        <Icon name={g.collapsed ? "expand" : "collapse"} size={14} />
                      </button>
                      <span className="name grow">{g.name}</span>
                      <span className="faint small">{members.length}</span>
                      <MenuButton
                        label={`${g.name} options`}
                        items={[
                          { label: "Rename", onClick: () => setRenaming({ id: g.id, name: g.name }) },
                          { label: "Delete group (keeps channels)", danger: true, onClick: () => request("deleteGroup", { id: g.id }).catch(() => {}) },
                        ]}
                      />
                    </div>
                    <div className="group-body">
                      <div className={`strip${g.mute ? " offline" : ""}`} style={{ width: 84 }} title="Group fader scales every member channel">
                        <div className="strip-sub"><Icon name="group" size={12} /> Group</div>
                        <div className="strip-body">
                          <Fader value={g.volume} label={`${g.name} group level`} muted={g.mute} onChange={(v) => void request("updateGroup", { id: g.id, volume: Math.round(v * 1000) / 1000 }).catch(() => {})} />
                        </div>
                        <div className="strip-value">{Math.round(g.volume * 100)}%</div>
                        <div className="strip-foot">
                          <button className={`btn small mute${g.mute ? " on" : ""}`} onClick={() => request("updateGroup", { id: g.id, mute: !g.mute }).catch(() => {})}>M</button>
                        </div>
                      </div>
                      {!g.collapsed && members.map(strip)}
                      {members.length === 0 && <div className="faint small" style={{ alignSelf: "center", padding: 12, width: 120 }}>Drag channels here</div>}
                    </div>
                  </div>
                );
              })}
              {ungrouped.map(strip)}
              <button className="add-strip" onClick={() => setAddOpen(true)}
                onDragOver={(e) => (e.preventDefault(), dragId && setDropTarget("end"))}
                onDrop={(e) => (e.preventDefault(), drop(null, ""))}>
                <Icon name="plus" size={22} />
                <span className="small">Add channel</span>
              </button>
            </div>
          </>
        )}

        {view !== "simple" && (
          <>
            <div className="section-label">
              Buses <span className="faint">({p.buses.length})</span>
              <button className="btn small ghost" onClick={() => request("createBus", { name: `Bus ${p.buses.length}` }).catch(() => {})}>
                <Icon name="plus" size={12} /> New bus
              </button>
            </div>
            <div className="strips">
              {p.buses.map((b) => (
                <BusStrip key={b.id} bus={b} runtime={state.runtime.buses[b.id]} devices={state.devices} onEffects={() => setFx({ target: "bus", id: b.id })} />
              ))}
            </div>
          </>
        )}
      </div>

      <AvailableApps state={state} icons={icons} searchRef={searchRef} />

      {fx && fxEntity && <EffectsEditor target={fx.target} id={fx.id} name={fxEntity.name} effects={fxEntity.effects} onClose={() => setFx(null)} />}
      {rules && <MatchRules channel={p.channels.find((c) => c.id === rules)!} onClose={() => setRules(null)} />}
      {newGroupFor !== undefined && (
        <Prompt
          title="New group"
          label="Group name"
          initial="Games"
          submitLabel="Create"
          onClose={() => setNewGroupFor(undefined)}
          onSubmit={(name) => request("createGroup", { name, channelIds: newGroupFor ? [newGroupFor] : [] }).catch(() => {})}
        />
      )}
      {renaming && (
        <Prompt title="Rename group" label="Group name" initial={renaming.name} onClose={() => setRenaming(null)} onSubmit={(name) => request("updateGroup", { id: renaming.id, name }).catch(() => {})} />
      )}
      {addOpen && <AddChannel state={state} onClose={() => setAddOpen(false)} />}
    </div>
  );

}

function MatchRules({ channel, onClose }: { channel: Channel; onClose: () => void }) {
  const [names, setNames] = useState(channel.match.exeNames.join(", "));
  const [path, setPath] = useState(channel.match.pathContains);
  const save = () => {
    const exeNames = names.split(",").map((s) => s.trim().toLowerCase()).filter(Boolean);
    request("updateChannel", { id: channel.id, exeNames, pathContains: path }).then(onClose).catch(() => {});
  };
  return (
    <Modal title={`Matching rules — ${channel.name}`} onClose={onClose} actions={<><button className="btn" onClick={onClose}>Cancel</button><button className="btn primary" onClick={save}>Save</button></>}>
      <p className="muted small">
        This channel controls every Windows audio session whose executable matches. Apps that use several processes (browsers, Discord) are covered
        automatically. When the app restarts, the channel reconnects by these rules.
      </p>
      <label className="col" style={{ marginBottom: 10 }}>
        <span className="small muted">Executable names (comma separated)</span>
        <input type="text" value={names} onChange={(e) => setNames(e.target.value)} placeholder="discord.exe" />
      </label>
      <label className="col">
        <span className="small muted">Only if the path contains (optional)</span>
        <input type="text" value={path} onChange={(e) => setPath(e.target.value)} placeholder="e.g. DiscordPTB" />
      </label>
    </Modal>
  );
}

function AddChannel({ state, onClose }: { state: EngineState; onClose: () => void }) {
  const captures = state.devices.filter((d) => d.flow === "capture" && d.state === "active");
  const renders = state.devices.filter((d) => d.flow === "render" && d.state === "active");
  const used = useMemo(() => new Set(state.profile.channels.map((c) => c.deviceId)), [state.profile.channels]);
  const add = (args: Record<string, unknown>) => request("createChannel", args).then(onClose).catch(() => {});
  const main = state.profile.buses.find((b) => b.isMain);
  return (
    <Modal title="Add channel" onClose={onClose} wide>
      <p className="muted small" style={{ marginTop: 0 }}>New channels are routed to the Main bus. Applications are added from the Available Apps panel.</p>
      <div className="grid2">
        <div className="card">
          <h3><Icon name="mic" size={14} /> Microphone or input device</h3>
          <p className="muted small">Captured by the engine (managed audio): volume, effects and routing to any bus.</p>
          <div className="list">
            {captures.map((d) => (
              <div className="list-row" key={d.id}>
                <span className="grow ellipsis" title={d.name}>{d.name}</span>
                {d.likelyVirtual && <span className="badge">virtual</span>}
                <button className="btn small" disabled={used.has(d.id)} onClick={() => add({ type: "hardwareInput", deviceId: d.id })}>{used.has(d.id) ? "Added" : "Add"}</button>
              </div>
            ))}
            {captures.length === 0 && <span className="faint">No recording devices found.</span>}
          </div>
        </div>
        <div className="card">
          <h3><Icon name="loop" size={14} /> Capture a playback device</h3>
          <p className="muted small">
            Records what plays on an output (loopback). Ideal for virtual cables: set an app's output to the cable in Windows, then capture the cable
            here. A device cannot be captured and played back into itself.
          </p>
          <div className="list">
            {renders.map((d) => (
              <div className="list-row" key={d.id}>
                <span className="grow ellipsis" title={d.name}>{d.name}</span>
                {d.likelyVirtual && <span className="badge">virtual</span>}
                <button className="btn small" disabled={used.has(d.id) || main?.deviceId === d.id} title={main?.deviceId === d.id ? "This is the Main output: capturing it into Main would create feedback" : undefined} onClick={() => add({ type: "endpointLoopback", deviceId: d.id })}>Add</button>
              </div>
            ))}
          </div>
        </div>
        <div className="card">
          <h3><Icon name="tone" size={14} /> Test tone</h3>
          <p className="muted small">A generated sine wave for checking routing and levels. Starts at a low level.</p>
          <div className="row">
            <button className="btn small" onClick={() => add({ type: "testTone", toneHz: 440 })}>440 Hz</button>
            <button className="btn small" onClick={() => add({ type: "testTone", toneHz: 1000 })}>1 kHz</button>
          </div>
        </div>
      </div>
    </Modal>
  );
}
