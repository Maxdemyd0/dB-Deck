// Engine connection store. The UI never invents engine state: everything
// rendered comes from `state` (pushed by the engine after it confirms a
// change) or from command responses.

import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import { useSyncExternalStore } from "react";
import type { EngineError, EngineState, Meters, WindowsVolume } from "./types";

type Listener = () => void;

interface Snapshot {
  state: EngineState | null;
  connected: boolean;
  message: string;
  icons: Record<string, string>;
}

let snapshot: Snapshot = { state: null, connected: false, message: "Connecting to audio engine…", icons: {} };
const listeners = new Set<Listener>();
let iconsVersion = -1;

function set(patch: Partial<Snapshot>) {
  snapshot = { ...snapshot, ...patch };
  listeners.forEach((l) => l());
}

// Meters update at ~30 Hz; components read them imperatively (see useMeter)
// so they never re-render the React tree.
export let meters: Meters = { channels: {}, buses: {} };
const meterListeners = new Set<() => void>();
export function onMeters(fn: () => void) {
  meterListeners.add(fn);
  return () => void meterListeners.delete(fn);
}

// --- toasts -------------------------------------------------------------------
export interface Toast {
  id: number;
  kind: "error" | "info" | "warn";
  text: string;
}
let toasts: Toast[] = [];
const toastListeners = new Set<Listener>();
let toastSeq = 1;
export function toast(kind: Toast["kind"], text: string) {
  const t = { id: toastSeq++, kind, text };
  toasts = [...toasts.filter((x) => x.text !== text), t].slice(-4);
  toastListeners.forEach((l) => l());
  setTimeout(() => dismissToast(t.id), kind === "error" ? 7000 : 4000);
}
export function dismissToast(id: number) {
  toasts = toasts.filter((t) => t.id !== id);
  toastListeners.forEach((l) => l());
}
export function useToasts() {
  return useSyncExternalStore(
    (l) => (toastListeners.add(l), () => void toastListeners.delete(l)),
    () => toasts,
  );
}

// --- requests -----------------------------------------------------------------
export const inTauri = typeof window !== "undefined" && "__TAURI_INTERNALS__" in window;

export async function request<T = unknown>(cmd: string, args: Record<string, unknown> = {}, quiet = false): Promise<T> {
  if (!inTauri) throw { code: "no-host", message: "dB-Deck UI must run inside the desktop app" } as EngineError;
  try {
    return (await invoke("engine_request", { cmd, args })) as T;
  } catch (e) {
    const err = (e && typeof e === "object" ? e : { code: "internal", message: String(e) }) as EngineError;
    if (!quiet) toast("error", err.message || "Command failed");
    throw err;
  }
}

async function refreshIcons() {
  try {
    const r = await request<{ version: number; icons: Record<string, string> }>("getIcons", {}, true);
    iconsVersion = r.version;
    set({ icons: r.icons });
  } catch {
    /* engine not ready */
  }
}

function applyState(state: EngineState) {
  set({ state, connected: true });
  if (state.engine.iconsVersion !== iconsVersion) void refreshIcons();
}

let started = false;
export function startEngineStore() {
  if (started || !inTauri) {
    if (!inTauri) set({ message: "Open dB-Deck from the desktop app to connect to the audio engine." });
    return;
  }
  started = true;
  void listen<{ event: string; data: unknown }>("engine-event", (e) => {
    const { event, data } = e.payload;
    if (event === "state") applyState(data as EngineState);
    else if (event === "meters") {
      meters = data as Meters;
      meterListeners.forEach((l) => l());
    } else if (event === "windowsVolume" && snapshot.state) {
      // Pushed immediately on any endpoint volume change (Quick Settings, keys, dB-Deck).
      set({ state: { ...snapshot.state, windowsVolume: data as WindowsVolume } });
    }
  });
  void listen<{ connected: boolean; message: string }>("engine-status", (e) => {
    set({ connected: e.payload.connected, message: e.payload.message });
  });
  // Initial: in case the connection was established before the listeners.
  void invoke<{ connected: boolean; message: string }>("engine_status").then(async (s) => {
    set({ connected: s.connected, message: s.message || snapshot.message });
    if (s.connected) {
      try {
        applyState(await request<EngineState>("getState", {}, true));
      } catch {
        /* ignore */
      }
    }
  });
}

export function useEngine(): Snapshot {
  return useSyncExternalStore(
    (l) => (listeners.add(l), () => void listeners.delete(l)),
    () => snapshot,
  );
}
