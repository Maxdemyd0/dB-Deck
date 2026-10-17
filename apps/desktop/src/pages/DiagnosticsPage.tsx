import { useEffect, useState } from "react";
import { request } from "../engine/store";
import type { EngineState, StreamStats } from "../engine/types";

interface Diagnostics {
  engineVersion: string;
  sampleRate: number;
  bufferMode: string;
  clock: string;
  mixer: { blocks: number; frames: number; lastBlockUs: number; maxBlockUs: number };
  outputs: { deviceId: string; deviceName: string; status: string; error: string; master: boolean; latencyMs: number; bufferFrames: number; periodFrames: number; format: string; glitches: number; ring?: StreamStats }[];
  inputs: { channelId: string; channelName: string; kind: string; pid: number; format: string; deviceBufferMs: number; ringTargetMs: number; stream: StreamStats; status: string }[];
  busOrder: { id: string; name: string; device: string }[];
  ipcClients: number;
  configDir: string;
  uptimeSec: number;
  latencyNote: string;
}

export function DiagnosticsPage({ state }: { state: EngineState }) {
  const [d, setD] = useState<Diagnostics | null>(null);
  useEffect(() => {
    let alive = true;
    const tick = () => request<Diagnostics>("getDiagnostics", {}, true).then((r) => alive && setD(r)).catch(() => {});
    tick();
    const t = setInterval(tick, 1000);
    return () => {
      alive = false;
      clearInterval(t);
    };
  }, []);
  if (!d) return <div className="page"><h1 className="page-title">Diagnostics</h1><p className="muted">Loading…</p></div>;
  const mainOut = d.outputs.find((o) => o.master);
  return (
    <div className="page wide">
      <h1 className="page-title">Diagnostics</h1>
      <p className="page-sub">Live figures reported by the engine. {d.latencyNote}</p>
      <div className="grid2">
        <div className="card">
          <h3>Engine</h3>
          <table className="tbl"><tbody>
            <tr><td className="muted">Version</td><td>{d.engineVersion}</td></tr>
            <tr><td className="muted">Process</td><td>PID {state.engine.pid}, up {Math.round(d.uptimeSec)} s</td></tr>
            <tr><td className="muted">Mixer clock</td><td>{d.clock === "main-output" ? "Main output device" : "Internal timer (Main not playing)"}</td></tr>
            <tr><td className="muted">Format</td><td>{d.sampleRate} Hz float32 stereo · buffering “{d.bufferMode}”</td></tr>
            <tr><td className="muted">Blocks processed</td><td>{d.mixer.blocks.toLocaleString()} ({(d.mixer.frames / d.sampleRate).toFixed(0)} s of audio)</td></tr>
            <tr><td className="muted">Mix time per block</td><td>{d.mixer.lastBlockUs.toFixed(1)} µs (max {d.mixer.maxBlockUs.toFixed(0)} µs)</td></tr>
            <tr><td className="muted">UI connections</td><td>{d.ipcClients}</td></tr>
            <tr><td className="muted">Settings folder</td><td className="mono" style={{ userSelect: "text" }}>{d.configDir}</td></tr>
          </tbody></table>
        </div>
        <div className="card">
          <h3>Latency estimate</h3>
          {mainOut ? (
            <table className="tbl"><tbody>
              <tr><td className="muted">Main output buffer</td><td>{mainOut.latencyMs.toFixed(1)} ms ({mainOut.bufferFrames} frames, period {mainOut.periodFrames})</td></tr>
              {d.inputs.map((i) => (
                <tr key={i.channelId + i.pid}><td className="muted">{i.channelName} → Main</td><td>≈ {(i.deviceBufferMs + i.ringTargetMs + mainOut.latencyMs).toFixed(1)} ms (capture {i.deviceBufferMs.toFixed(1)} + ring {i.ringTargetMs.toFixed(1)} + output)</td></tr>
              ))}
            </tbody></table>
          ) : (
            <p className="muted">Main is not playing on a device, so no output latency applies.</p>
          )}
        </div>
      </div>
      <div className="card">
        <h3>Outputs</h3>
        <table className="tbl">
          <thead><tr><th>Device</th><th>Role</th><th>Status</th><th>Format</th><th>Buffer</th><th>Drift ring</th><th>Stalls</th></tr></thead>
          <tbody>
            {d.outputs.map((o) => (
              <tr key={o.deviceId}>
                <td>{o.deviceName}</td>
                <td>{o.master ? "Clock (Main)" : "Follower"}</td>
                <td>{o.status}{o.error && <div className="faint small">{o.error}</div>}</td>
                <td className="small">{o.format}</td>
                <td>{o.latencyMs ? `${o.latencyMs.toFixed(1)} ms` : "—"}</td>
                <td className="small">{o.ring ? `fill ${o.ring.fillFrames}/${o.ring.targetFrames}, ratio ${o.ring.ratio.toFixed(5)}, underruns ${o.ring.underruns}` : "—"}</td>
                <td>{o.glitches}</td>
              </tr>
            ))}
            {d.outputs.length === 0 && <tr><td colSpan={7} className="faint">No output devices are open.</td></tr>}
          </tbody>
        </table>
      </div>
      <div className="card">
        <h3>Inputs</h3>
        <table className="tbl">
          <thead><tr><th>Channel</th><th>Kind</th><th>Format</th><th>Capture buffer</th><th>Ring</th><th>Underruns</th><th>Overruns</th><th>Ratio</th></tr></thead>
          <tbody>
            {d.inputs.map((i) => (
              <tr key={i.channelId + i.pid}>
                <td>{i.channelName}{i.pid ? <span className="faint small"> (pid {i.pid})</span> : null}</td>
                <td>{i.kind}</td>
                <td className="small">{i.format}</td>
                <td>{i.deviceBufferMs.toFixed(1)} ms</td>
                <td className="small">{i.stream.starving ? "waiting for audio" : `${i.stream.fillFrames}/${i.stream.targetFrames}`}</td>
                <td>{i.stream.underruns}</td>
                <td>{i.stream.overruns}</td>
                <td className="mono">{i.stream.ratio.toFixed(5)}</td>
              </tr>
            ))}
            {d.inputs.length === 0 && <tr><td colSpan={8} className="faint">No inputs are being captured.</td></tr>}
          </tbody>
        </table>
      </div>
      <div className="card">
        <h3>Audio graph</h3>
        <p className="muted small" style={{ marginTop: 0 }}>Bus processing order (senders before receivers):</p>
        <div className="row wrap">
          {d.busOrder.map((b, i) => (
            <span key={b.id} className="row" style={{ gap: 6 }}>
              {i > 0 && <span className="faint">→</span>}
              <span className="badge">{b.name}{b.device ? "" : " (no device)"}</span>
            </span>
          ))}
        </div>
      </div>
      <div className="card">
        <h3>Virtual driver</h3>
        <p className="muted" style={{ margin: 0 }}>{state.driver.message}</p>
      </div>
    </div>
  );
}
