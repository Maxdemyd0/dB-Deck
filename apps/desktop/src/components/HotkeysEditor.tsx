import { useState } from "react";
import { acceleratorFromEvent, duplicateAccelerators, formatAccelerator, HOTKEY_ACTIONS, useHotkeyFailures } from "../engine/hotkeys";
import { request } from "../engine/store";
import type { EngineState, Hotkey, HotkeyAction } from "../engine/types";
import { Icon } from "./Icon";

/** Settings card: global hotkeys that work while dB-Deck is hidden or a game has focus. */
export function HotkeysEditor({ state }: { state: EngineState }) {
  const keys = state.settings.hotkeys ?? [];
  const failed = useHotkeyFailures();
  const dups = duplicateAccelerators(keys);
  const [capturing, setCapturing] = useState<number | null>(null); // index, or -1 for a new binding

  const save = (next: Hotkey[]) => request("updateSettings", { hotkeys: next }).catch(() => {});
  const setAt = (i: number, patch: Partial<Hotkey>) => save(keys.map((k, j) => (j === i ? { ...k, ...patch } : k)));

  const strips = [
    ...state.profile.channels.map((c) => ({ id: c.id, name: c.name })),
    ...state.profile.buses.map((b) => ({ id: b.id, name: `${b.name} (bus)` })),
  ];
  const targetsFor = (action: HotkeyAction) => {
    const t = HOTKEY_ACTIONS.find((a) => a.action === action)?.target;
    if (t === "strip") return strips;
    if (t === "scene") return state.profile.scenes.map((s) => ({ id: s.id, name: s.name }));
    if (t === "profile") return state.profiles;
    return null;
  };
  const defaultTarget = (action: HotkeyAction) => targetsFor(action)?.[0]?.id ?? "";

  const onCapture = (e: React.KeyboardEvent) => {
    e.preventDefault();
    if (e.key === "Escape") return setCapturing(null);
    const acc = acceleratorFromEvent(e);
    if (!acc) return;
    if (capturing === -1) {
      const action: HotkeyAction = strips.length ? "toggleMute" : "masterMute";
      void save([...keys, { accelerator: acc, action, targetId: action === "masterMute" ? "" : defaultTarget(action) }]);
    } else if (capturing !== null) {
      void setAt(capturing, { accelerator: acc });
    }
    setCapturing(null);
  };

  const keyButton = (i: number, acc?: string) => (
    <button
      className={`btn small hotkey-capture${capturing === i ? " on" : ""}`}
      onClick={() => setCapturing(i)}
      onKeyDown={capturing === i ? onCapture : undefined}
      onBlur={() => capturing === i && setCapturing(null)}
      title="Click, then press the key combination"
    >
      {capturing === i ? "Press keys…" : acc ? <kbd>{formatAccelerator(acc)}</kbd> : "Set keys"}
    </button>
  );

  return (
    <div className="card">
      <h3>Global hotkeys</h3>
      <p className="faint small" style={{ marginTop: 0 }}>
        These work anywhere, even in a game or with dB-Deck in the tray. Use at least one modifier (Ctrl, Alt, Shift, Win), or an F-key.
      </p>
      <div className="list">
        {keys.map((k, i) => {
          const targets = targetsFor(k.action);
          const problem = failed.includes(k.accelerator)
            ? "Another app already uses this combination."
            : dups.has(k.accelerator.toLowerCase())
              ? "Used more than once; all its actions run."
              : targets && !targets.some((t) => t.id === k.targetId)
                ? "Its target no longer exists."
                : "";
          return (
            <div className="list-row hotkey-row" key={i}>
              {keyButton(i, k.accelerator)}
              <select
                value={k.action}
                aria-label="Action"
                onChange={(e) => {
                  const action = e.target.value as HotkeyAction;
                  void setAt(i, { action, targetId: action === "masterMute" ? "" : targetsFor(action)?.some((t) => t.id === k.targetId) ? k.targetId : defaultTarget(action) });
                }}
              >
                {HOTKEY_ACTIONS.map((a) => (
                  <option key={a.action} value={a.action}>{a.label}</option>
                ))}
              </select>
              {targets && (
                <select value={k.targetId} aria-label="Target" onChange={(e) => void setAt(i, { targetId: e.target.value })}>
                  {!targets.some((t) => t.id === k.targetId) && <option value={k.targetId}>(missing)</option>}
                  {targets.map((t) => (
                    <option key={t.id} value={t.id}>{t.name}</option>
                  ))}
                </select>
              )}
              <span className="grow">{problem && <span className="badge warn" title={problem}><Icon name="warn" size={11} /> {problem}</span>}</span>
              <button className="btn ghost icon small" aria-label="Remove hotkey" title="Remove" onClick={() => void save(keys.filter((_, j) => j !== i))}>
                <Icon name="close" size={14} />
              </button>
            </div>
          );
        })}
        <div className="list-row">
          {capturing === -1 ? keyButton(-1) : (
            <button className="btn small" onClick={() => setCapturing(-1)}>
              <Icon name="plus" size={12} /> Add hotkey
            </button>
          )}
        </div>
      </div>
    </div>
  );
}
