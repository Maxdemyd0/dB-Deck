import { Icon } from "../components/Icon";
import { Meter } from "../components/Meter";
import { WindowsVolume } from "../components/WindowsVolume";
import { busStatus } from "../components/BusStrip";
import { request } from "../engine/store";
import type { Device, EngineState } from "../engine/types";

function DeviceState({ d }: { d: Device }) {
  return (
    <>
      {d.isDefault && <span className="badge accent" title="Windows default device">Windows default</span>}
      {d.likelyVirtual && <span className="badge" title="Name suggests a virtual device">virtual</span>}
      {d.state !== "active" && <span className="badge warn">{d.state}</span>}
    </>
  );
}

export function InputsPage({ state }: { state: EngineState }) {
  const p = state.profile;
  const inputs = p.channels.filter((c) => c.type !== "app");
  const capture = state.devices.filter((d) => d.flow === "capture");
  const used = new Set(p.channels.map((c) => c.deviceId));
  return (
    <div className="page">
      <h1 className="page-title">Inputs</h1>
      <p className="page-sub">Microphones and other recording devices are captured by the engine, so their audio can be processed and routed to any bus.</p>
      <div className="card">
        <h3>Input channels in this profile</h3>
        {inputs.length === 0 && <span className="faint">No input channels yet.</span>}
        <div className="list">
          {inputs.map((c) => {
            const rt = state.runtime.channels[c.id];
            return (
              <div className="list-row" key={c.id}>
                <Icon name={c.type === "hardwareInput" ? "mic" : c.type === "testTone" ? "tone" : "loop"} />
                <div className="grow">
                  <div className="ellipsis">{c.name}</div>
                  <div className="faint small ellipsis">
                    {c.type === "testTone" ? `Generated ${c.toneHz} Hz tone` : c.deviceName}
                    {rt?.captures[0] && ` · ${rt.captures[0].format}`}
                    {rt?.error && rt.status !== "connected" && ` · ${rt.error}`}
                  </div>
                </div>
                <div style={{ width: 120 }}><Meter kind="channels" id={c.id} horizontal /></div>
                <span className={`badge ${rt?.status === "connected" || rt?.status === "generated" ? "ok" : rt?.status === "disconnected" ? "danger" : ""}`}>{rt?.status ?? "—"}</span>
              </div>
            );
          })}
        </div>
      </div>
      <div className="card">
        <h3>Recording devices</h3>
        <div className="list">
          {capture.map((d) => (
            <div className="list-row" key={d.id}>
              <Icon name="mic" />
              <span className="grow ellipsis" title={d.name}>{d.name}</span>
              <DeviceState d={d} />
              <span className="faint small">{d.format}</span>
              <button className="btn small" disabled={d.state !== "active" || used.has(d.id)} onClick={() => request("createChannel", { type: "hardwareInput", deviceId: d.id }).catch(() => {})}>
                {used.has(d.id) ? "Added" : "Add channel"}
              </button>
            </div>
          ))}
        </div>
      </div>
    </div>
  );
}

export function OutputsPage({ state }: { state: EngineState }) {
  const p = state.profile;
  const main = p.buses.find((b) => b.isMain)!;
  const renders = state.devices.filter((d) => d.flow === "render");
  const active = renders.filter((d) => d.state === "active");
  const mainRt = state.runtime.buses[main.id];
  const st = busStatus(mainRt);
  const nameOf = (id: string) => state.devices.find((d) => d.id === id)?.name ?? "Unavailable device";
  return (
    <div className="page">
      <h1 className="page-title">Outputs</h1>
      <p className="page-sub">
        Main is dB-Deck's primary mix. It plays on the device you choose here — independent of the Windows default — and never switches to another device unless you add it as a fallback.
      </p>

      <div className="card">
        <h3>Main output</h3>
        <div className="row wrap">
          <select value={main.deviceId} onChange={(e) => request("setMainOutput", { deviceId: e.target.value }).catch(() => {})} style={{ minWidth: 320 }}>
            <option value="">— None (Main is silent) —</option>
            {main.deviceId && !active.some((d) => d.id === main.deviceId) && <option value={main.deviceId}>{main.deviceName || "Saved device"} (disconnected)</option>}
            {active.map((d) => <option key={d.id} value={d.id}>{d.name}</option>)}
          </select>
          <span className={`dot ${st.cls}`} /> <span className="muted">{st.text}</span>
          {mainRt?.outputLatencyMs ? <span className="faint small">output buffer ≈ {mainRt.outputLatencyMs.toFixed(1)} ms</span> : null}
        </div>
        {mainRt?.status === "disconnected" && (
          <div className="notice danger" style={{ marginTop: 10 }}>
            <Icon name="warn" />
            <div>
              <strong>{main.deviceName || "The Main output"} is disconnected.</strong> Managed audio to Main is stopped and your routing is kept. It reconnects automatically when the device returns
              {main.fallbackDeviceIds.length ? ", or plays on a fallback you authorised below." : ". Add a fallback below if you want audio to continue elsewhere."}
            </div>
          </div>
        )}
        <div style={{ marginTop: 14 }}>
          <div className="muted small" style={{ marginBottom: 6 }}>Authorised fallbacks (used in order only while the Main device is unavailable)</div>
          {main.fallbackDeviceIds.map((id, i) => (
            <div className="list-row" key={id}>
              <span className="faint mono">{i + 1}</span>
              <span className="grow ellipsis">{nameOf(id)}</span>
              <button className="btn small ghost danger" onClick={() => request("updateBus", { id: main.id, fallbackDeviceIds: main.fallbackDeviceIds.filter((x) => x !== id) }).catch(() => {})}>Remove</button>
            </div>
          ))}
          <select value="" onChange={(e) => e.target.value && request("updateBus", { id: main.id, fallbackDeviceIds: [...main.fallbackDeviceIds, e.target.value] }).catch(() => {})}>
            <option value="">Add fallback device…</option>
            {active.filter((d) => d.id !== main.deviceId && !main.fallbackDeviceIds.includes(d.id)).map((d) => <option key={d.id} value={d.id}>{d.name}</option>)}
          </select>
        </div>
      </div>

      <div className="card">
        <h3>Windows volume</h3>
        <p className="muted small" style={{ marginTop: 0 }}>
          These control the Windows endpoint volume (the same one as Quick Settings and the keyboard volume keys). They are separate from the Main bus level inside dB-Deck.
        </p>
        <div className="col">
          <div className="row"><span className="muted small" style={{ width: 150 }}>Windows default device</span><WindowsVolume ep={state.windowsVolume.default} target="default" /></div>
          <div className="row"><span className="muted small" style={{ width: 150 }}>Main output device</span><WindowsVolume ep={state.windowsVolume.main} target="main" /></div>
        </div>
      </div>

      <div className="card">
        <h3>Buses</h3>
        <div className="list">
          {p.buses.map((b) => {
            const rt = state.runtime.buses[b.id];
            const s = busStatus(rt);
            return (
              <div className="list-row" key={b.id}>
                <Icon name={b.isMain ? "output" : "bus"} />
                <strong style={{ width: 120 }} className="ellipsis">{b.name}</strong>
                <select value={b.deviceId} disabled={b.isMain} onChange={(e) => request("updateBus", { id: b.id, deviceId: e.target.value }).catch(() => {})} style={{ minWidth: 260 }} title={b.isMain ? "Choose Main's device above" : undefined}>
                  <option value="">— No output —</option>
                  {b.deviceId && !active.some((d) => d.id === b.deviceId) && <option value={b.deviceId}>{b.deviceName} (disconnected)</option>}
                  {active.map((d) => <option key={d.id} value={d.id}>{d.name}</option>)}
                </select>
                <span className={`dot ${s.cls}`} />
                <span className="grow faint small ellipsis">{s.text}</span>
                <div style={{ width: 110 }}><Meter kind="buses" id={b.id} horizontal /></div>
                {!b.isMain && <button className="btn small ghost danger" onClick={() => request("deleteBus", { id: b.id }).catch(() => {})}>Delete</button>}
              </div>
            );
          })}
        </div>
        <button className="btn small" style={{ marginTop: 10 }} onClick={() => request("createBus", { name: `Bus ${p.buses.length}` }).catch(() => {})}>
          <Icon name="plus" size={12} /> New bus
        </button>
      </div>

      <div className="card">
        <h3>Playback devices</h3>
        <div className="list">
          {renders.map((d) => (
            <div className="list-row" key={d.id}>
              <Icon name="output" />
              <span className="grow ellipsis" title={d.name}>{d.name}</span>
              <DeviceState d={d} />
              <span className="faint small">{d.format}</span>
            </div>
          ))}
        </div>
      </div>
    </div>
  );
}

function VirtualEndpointRow({ d, used }: { d: Device; used: Set<string> }) {
  return (
    <div className="list-row">
      <Icon name={d.flow === "render" ? "output" : "mic"} />
      <span className="grow ellipsis" title={d.name}>{d.name}</span>
      <span className="badge">{d.flow === "render" ? "playback" : "recording"}</span>
      {d.state !== "active" && <span className="badge warn">{d.state}</span>}
      {d.flow === "render" ? (
        <>
          <button className="btn small" disabled={used.has(d.id) || d.state !== "active"} title="Capture what apps play into this device" onClick={() => request("createChannel", { type: "endpointLoopback", deviceId: d.id }).catch(() => {})}>Capture as input</button>
          <button className="btn small" disabled={d.state !== "active"} title="Send a dB-Deck bus into this device" onClick={() => request("createBus", { name: d.name.split(" (")[0], deviceId: d.id }).catch(() => {})}>New bus to it</button>
        </>
      ) : (
        <button className="btn small" disabled={used.has(d.id) || d.state !== "active"} onClick={() => request("createChannel", { type: "hardwareInput", deviceId: d.id }).catch(() => {})}>Add as input</button>
      )}
    </div>
  );
}

export function VirtualDevicesPage({ state }: { state: EngineState }) {
  const drv = state.driver;
  const used = new Set(state.profile.channels.map((c) => c.deviceId));
  const nameOf = (id: string) => state.devices.find((d) => d.id === id)?.name ?? id;
  return (
    <div className="page">
      <h1 className="page-title">Virtual Devices</h1>
      <p className="page-sub">Virtual devices let other apps send audio into dB-Deck, or record a dB-Deck bus (for example in OBS or Discord).</p>

      {drv.installed ? (
        <div className="card">
          <h3><Icon name="virtual" size={14} /> dB-Deck Virtual Cable <span className="badge ok">installed</span></h3>
          <p className="muted small" style={{ marginTop: 0 }}>{drv.message}</p>
          <div className="list">{drv.endpoints.map((d) => <VirtualEndpointRow key={d.id} d={d} used={used} />)}</div>
        </div>
      ) : (
        <div className="notice warn">
          <Icon name="warn" />
          <div><strong>dB-Deck's virtual cable driver is not installed.</strong> {drv.message}</div>
        </div>
      )}

      <div className="card" style={{ marginTop: 12 }}>
        <h3>How to route with a virtual cable</h3>
        <ol className="muted" style={{ margin: "0 0 4px", paddingLeft: 18, lineHeight: 1.7 }}>
          <li><strong>App → dB-Deck:</strong> in Windows Settings › Sound › Volume mixer, set the app's output to the cable's playback side. Then use <em>Capture as input</em> on that playback side.</li>
          <li><strong>dB-Deck → app:</strong> use <em>New bus to it</em> on the cable's playback side, and pick the cable's recording side in OBS or Discord.</li>
        </ol>
        {drv.cablePairs.length > 0 && (
          <p className="faint small" style={{ marginBottom: 0 }}>
            Recognised cable pairs (used for feedback protection): {drv.cablePairs.map((p) => `${nameOf(p.playback)} → ${nameOf(p.recording)}`).join("; ")}
          </p>
        )}
      </div>

      <div className="card">
        <h3>Other virtual endpoints ({drv.thirdPartyVirtualEndpoints.length})</h3>
        {drv.thirdPartyVirtualEndpoints.length === 0 && <span className="faint">No other virtual audio devices were detected.</span>}
        <div className="list">{drv.thirdPartyVirtualEndpoints.map((d) => <VirtualEndpointRow key={d.id} d={d} used={used} />)}</div>
      </div>
    </div>
  );
}

export function RoutingPage({ state }: { state: EngineState }) {
  const p = state.profile;
  return (
    <div className="page wide">
      <h1 className="page-title">Routing matrix</h1>
      <p className="page-sub">Every channel can feed any number of buses at its own level. Buses can also feed other buses. dB-Deck rejects routes that would form a loop.</p>
      <div className="matrix">
        <table>
          <thead>
            <tr>
              <th>Channel</th>
              {p.buses.map((b) => <th key={b.id}>{b.name}</th>)}
            </tr>
          </thead>
          <tbody>
            {p.channels.map((c) => {
              const rt = state.runtime.channels[c.id];
              return (
                <tr key={c.id}>
                  <td>
                    <div className="row"><span className="ellipsis" style={{ maxWidth: 180 }}>{c.name}</span><span className={`badge ${rt?.capability === "managed" ? "accent" : ""}`}>{rt?.capability === "managed" ? "Managed" : "Windows"}</span></div>
                  </td>
                  {p.buses.map((b) => {
                    const s = c.sends.find((x) => x.busId === b.id);
                    const sr = rt?.sends[b.id];
                    return (
                      <td className="cell" key={b.id} title={sr?.reason}>
                        <div className="cellbox">
                          <input type="checkbox" checked={!!s?.enabled} onChange={() => request("setSend", { channelId: c.id, busId: b.id, enabled: !s?.enabled }).catch(() => {})} aria-label={`${c.name} to ${b.name}`} />
                          <input className="hslider" type="range" min={0} max={1.5} step={0.01} disabled={!s?.enabled} value={s?.gain ?? 1} style={{ width: 70 }} onChange={(e) => request("setSend", { channelId: c.id, busId: b.id, gain: Number(e.target.value) }, true).catch(() => {})} />
                          <label className="faint small" title="Pre-fader: ignores the channel fader">
                            <input type="checkbox" checked={!!s?.preFader} disabled={!s?.enabled} onChange={() => request("setSend", { channelId: c.id, busId: b.id, preFader: !s?.preFader }).catch(() => {})} /> pre
                          </label>
                          {sr?.state === "blocked" && <Icon name="warn" size={13} className="" />}
                          {sr?.state === "session-only" && s?.enabled && <span className="faint small">logical</span>}
                        </div>
                      </td>
                    );
                  })}
                </tr>
              );
            })}
          </tbody>
        </table>
      </div>

      <h3 style={{ marginTop: 22 }}>Bus to bus</h3>
      <div className="matrix">
        <table>
          <thead>
            <tr>
              <th>From \ To</th>
              {p.buses.map((b) => <th key={b.id}>{b.name}</th>)}
            </tr>
          </thead>
          <tbody>
            {p.buses.map((from) => (
              <tr key={from.id}>
                <td><strong>{from.name}</strong></td>
                {p.buses.map((to) => {
                  const s = from.busSends.find((x) => x.busId === to.id);
                  return (
                    <td className="cell" key={to.id}>
                      {from.id === to.id ? <span className="faint">—</span> : (
                        <div className="cellbox">
                          <input type="checkbox" checked={!!s?.enabled} onChange={() => request("setBusSend", { fromBusId: from.id, toBusId: to.id, enabled: !s?.enabled }).catch(() => {})} />
                          <input className="hslider" type="range" min={0} max={1.5} step={0.01} disabled={!s?.enabled} value={s?.gain ?? 1} style={{ width: 70 }} onChange={(e) => request("setBusSend", { fromBusId: from.id, toBusId: to.id, gain: Number(e.target.value) }, true).catch(() => {})} />
                        </div>
                      )}
                    </td>
                  );
                })}
              </tr>
            ))}
          </tbody>
        </table>
      </div>
      <p className="faint small">"Windows" channels only control Windows session volume; their sends are logical until app capture is enabled. Hover a cell for details.</p>
    </div>
  );
}
