import { useEffect, useState } from "react";
import { request } from "../engine/store";
import type { Effect } from "../engine/types";
import { Icon } from "./Icon";
import { Modal } from "./Overlay";

interface EffectType {
  type: Effect["type"];
  params: { name: string; min: number; max: number; default: number; unit: string }[];
}

const titles: Record<string, string> = { gain: "Input gain", eq: "3-band EQ", gate: "Noise gate", compressor: "Compressor", limiter: "Limiter" };
const labels: Record<string, string> = {
  gainDb: "Gain", lowFreq: "Low freq", lowGainDb: "Low gain", midFreq: "Mid freq", midGainDb: "Mid gain", midQ: "Mid Q",
  highFreq: "High freq", highGainDb: "High gain", thresholdDb: "Threshold", rangeDb: "Range", attackMs: "Attack",
  holdMs: "Hold", releaseMs: "Release", ratio: "Ratio", kneeDb: "Knee", makeupDb: "Makeup", ceilingDb: "Ceiling",
};

/** Edits a channel's or bus's effect chain. Processing happens in the engine on real PCM. */
export function EffectsEditor({ target, id, name, effects, onClose }: { target: "channel" | "bus"; id: string; name: string; effects: Effect[]; onClose: () => void }) {
  const [types, setTypes] = useState<EffectType[]>([]);
  useEffect(() => {
    request<EffectType[]>("getEffectTypes").then(setTypes).catch(() => {});
  }, []);
  const base = { target, id };
  return (
    <Modal title={`Effects — ${name}`} onClose={onClose} wide>
      <p className="muted small" style={{ marginTop: 0 }}>
        Effects run in the dB-Deck engine on this {target}'s audio, in order from top to bottom.
      </p>
      {effects.length === 0 && <div className="notice">No effects yet. Add one below.</div>}
      {effects.map((e, i) => {
        const spec = types.find((t) => t.type === e.type);
        return (
          <div className="fx-card" key={e.id}>
            <div className="row" style={{ marginBottom: 6 }}>
              <strong className="grow">{titles[e.type] ?? e.type}</strong>
              <button className={`btn small toggle${e.enabled ? " on" : ""}`} onClick={() => request("updateEffect", { ...base, effectId: e.id, enabled: !e.enabled }).catch(() => {})}>
                {e.enabled ? "On" : "Bypassed"}
              </button>
              <button className="btn small ghost" disabled={i === 0} onClick={() => request("moveEffect", { ...base, effectId: e.id, index: i - 1 }).catch(() => {})} title="Move up">↑</button>
              <button className="btn small ghost" disabled={i === effects.length - 1} onClick={() => request("moveEffect", { ...base, effectId: e.id, index: i + 1 }).catch(() => {})} title="Move down">↓</button>
              <button className="btn small ghost danger" onClick={() => request("removeEffect", { ...base, effectId: e.id }).catch(() => {})} title="Remove">
                <Icon name="close" size={14} />
              </button>
            </div>
            {spec?.params.map((ps) => {
              const v = e.params[ps.name] ?? ps.default;
              const step = ps.max - ps.min > 100 ? 1 : ps.max - ps.min > 10 ? 0.1 : 0.01;
              return (
                <div className="fx-param" key={ps.name}>
                  <span className="muted small">{labels[ps.name] ?? ps.name}</span>
                  <input
                    className="hslider"
                    type="range"
                    min={ps.min}
                    max={ps.max}
                    step={step}
                    value={v}
                    onChange={(ev) => request("updateEffect", { ...base, effectId: e.id, params: { [ps.name]: Number(ev.target.value) } }, true).catch(() => {})}
                    onDoubleClick={() => request("updateEffect", { ...base, effectId: e.id, params: { [ps.name]: ps.default } }).catch(() => {})}
                  />
                  <span className="mono small">{v.toFixed(step < 1 ? (step < 0.1 ? 2 : 1) : 0)} {ps.unit}</span>
                </div>
              );
            })}
          </div>
        );
      })}
      <div className="row wrap" style={{ marginTop: 14 }}>
        <span className="muted small">Add:</span>
        {types.map((t) => (
          <button key={t.type} className="btn small" onClick={() => request("addEffect", { ...base, type: t.type }).catch(() => {})}>
            <Icon name="plus" size={12} /> {titles[t.type] ?? t.type}
          </button>
        ))}
      </div>
    </Modal>
  );
}
