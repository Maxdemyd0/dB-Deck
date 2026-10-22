// Global hotkeys: registered with Windows through the Tauri global-shortcut
// plugin and run here, in the UI, which keeps running while the window is
// hidden in the tray. Bindings are stored in the engine's settings.

import { isRegistered, register, unregisterAll } from "@tauri-apps/plugin-global-shortcut";
import { useSyncExternalStore } from "react";
import { FADER_MAX, stepFaderDb } from "./format";
import { currentState, inTauri, request } from "./store";
import type { EngineState, Hotkey, HotkeyAction } from "./types";

export const HOTKEY_ACTIONS: { action: HotkeyAction; label: string; target: "strip" | "scene" | "profile" | null }[] = [
  { action: "toggleMute", label: "Toggle mute", target: "strip" },
  { action: "pushToMute", label: "Mute while held", target: "strip" },
  { action: "volumeUp", label: "Volume +1 dB", target: "strip" },
  { action: "volumeDown", label: "Volume −1 dB", target: "strip" },
  { action: "masterMute", label: "Mute all managed audio", target: null },
  { action: "applyScene", label: "Apply scene", target: "scene" },
  { action: "loadProfile", label: "Load profile", target: "profile" },
];

const MODIFIER_CODES = new Set(["ControlLeft", "ControlRight", "AltLeft", "AltRight", "ShiftLeft", "ShiftRight", "MetaLeft", "MetaRight"]);

/**
 * Accelerator for a key press ("Ctrl+Alt+KeyM"), or null while only modifiers
 * are down. A global hotkey needs at least one modifier, except F-keys.
 */
export function acceleratorFromEvent(e: { code: string; ctrlKey: boolean; altKey: boolean; shiftKey: boolean; metaKey: boolean }): string | null {
  if (MODIFIER_CODES.has(e.code) || !e.code) return null;
  const mods = [e.ctrlKey && "Ctrl", e.altKey && "Alt", e.shiftKey && "Shift", e.metaKey && "Super"].filter(Boolean) as string[];
  if (mods.length === 0 && !/^F\d{1,2}$/.test(e.code)) return null;
  return [...mods, e.code].join("+");
}

/** Readable form: "Ctrl+Alt+KeyM" → "Ctrl + Alt + M". */
export function formatAccelerator(acc: string): string {
  return acc
    .split("+")
    .map((p) => p.replace(/^Key([A-Z])$/, "$1").replace(/^Digit(\d)$/, "$1").replace(/^Numpad(\w+)$/, "Num $1").replace(/^Arrow(\w+)$/, "$1"))
    .join(" + ");
}

/** Accelerators that appear more than once (lower-case). */
export function duplicateAccelerators(keys: Hotkey[]): Set<string> {
  const seen = new Set<string>(),
    dup = new Set<string>();
  for (const k of keys) {
    const a = k.accelerator.toLowerCase();
    if (seen.has(a)) dup.add(a);
    seen.add(a);
  }
  return dup;
}

type Strip = { kind: "channel" | "bus"; id: string; volume: number; mute: boolean; max: number };

export function findStrip(state: EngineState, id: string): Strip | null {
  const c = state.profile.channels.find((x) => x.id === id);
  if (c) return { kind: "channel", id, volume: c.volume, mute: c.mute, max: c.type !== "app" || c.appCapture ? FADER_MAX : 1 };
  const b = state.profile.buses.find((x) => x.id === id);
  if (b) return { kind: "bus", id, volume: b.volume, mute: b.mute, max: FADER_MAX };
  return null;
}

/** The engine command a hotkey runs, or null when there is nothing to do (or its target is gone). */
export function hotkeyCommand(state: EngineState, k: Hotkey, pressed: boolean): { cmd: string; args: Record<string, unknown> } | null {
  if (k.action === "masterMute") return pressed ? { cmd: "setMasterMute", args: { mute: !state.engine.masterMute } } : null;
  if (k.action === "applyScene")
    return pressed && state.profile.scenes.some((s) => s.id === k.targetId) ? { cmd: "applyScene", args: { id: k.targetId } } : null;
  if (k.action === "loadProfile")
    return pressed && k.targetId !== state.activeProfileId && state.profiles.some((p) => p.id === k.targetId)
      ? { cmd: "loadProfile", args: { id: k.targetId } }
      : null;
  const s = findStrip(state, k.targetId);
  if (!s) return null;
  const cmd = s.kind === "channel" ? "updateChannel" : "updateBus";
  if (k.action === "pushToMute") return { cmd, args: { id: s.id, mute: pressed } };
  if (!pressed) return null;
  if (k.action === "toggleMute") return { cmd, args: { id: s.id, mute: !s.mute } };
  const v = stepFaderDb(s.volume, k.action === "volumeUp" ? 1 : -1, s.max);
  return { cmd, args: { id: s.id, volume: Math.round(v * 1000) / 1000 } };
}

let registeredSig = "";
let lastFailed: string[] = [];
const failListeners = new Set<() => void>();

/** Accelerators Windows refused at the last registration (taken by another app). */
export function useHotkeyFailures(): string[] {
  return useSyncExternalStore(
    (l) => (failListeners.add(l), () => void failListeners.delete(l)),
    () => lastFailed,
  );
}

/**
 * Registers the bindings with Windows, replacing earlier ones. Returns the
 * accelerators Windows refused, usually because another app already uses them.
 */
export async function syncHotkeys(keys: Hotkey[]): Promise<string[]> {
  if (!inTauri) return [];
  const sig = JSON.stringify(keys);
  if (sig === registeredSig) return lastFailed;
  registeredSig = sig;
  await unregisterAll().catch(() => {});
  const failed: string[] = [];
  const byAcc = new Map<string, Hotkey[]>();
  for (const k of keys) byAcc.set(k.accelerator, [...(byAcc.get(k.accelerator) ?? []), k]);
  for (const [acc, bound] of byAcc) {
    try {
      await register(acc, (ev) => {
        const state = currentState();
        if (!state) return;
        for (const k of bound) {
          const c = hotkeyCommand(state, k, ev.state === "Pressed");
          if (c) void request(c.cmd, c.args, true).catch(() => {});
        }
      });
      if (!(await isRegistered(acc))) failed.push(acc);
    } catch {
      failed.push(acc);
    }
  }
  lastFailed = failed;
  failListeners.forEach((l) => l());
  return failed;
}
