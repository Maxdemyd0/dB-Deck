import { describe, expect, it, vi } from "vitest";

vi.mock("@tauri-apps/plugin-global-shortcut", () => ({ register: vi.fn(), unregisterAll: vi.fn(), isRegistered: vi.fn() }));
vi.mock("@tauri-apps/api/core", () => ({ invoke: vi.fn() }));
vi.mock("@tauri-apps/api/event", () => ({ listen: vi.fn() }));

import { acceleratorFromEvent, duplicateAccelerators, formatAccelerator, hotkeyCommand } from "./hotkeys";
import { deviceChanges } from "./store";
import type { EngineState } from "./types";

const key = (code: string, mods: Partial<{ ctrlKey: boolean; altKey: boolean; shiftKey: boolean; metaKey: boolean }> = {}) => ({
  code,
  ctrlKey: false,
  altKey: false,
  shiftKey: false,
  metaKey: false,
  ...mods,
});

const state = {
  engine: { masterMute: false },
  activeProfileId: "p1",
  profiles: [
    { id: "p1", name: "A" },
    { id: "p2", name: "B" },
  ],
  profile: {
    channels: [{ id: "mic", type: "hardwareInput", appCapture: false, volume: 1, mute: false }],
    buses: [{ id: "main", volume: 1, mute: true }],
    scenes: [{ id: "s1", name: "Night" }],
  },
} as unknown as EngineState;

describe("hotkeys", () => {
  it("builds accelerators from key presses", () => {
    expect(acceleratorFromEvent(key("KeyM", { ctrlKey: true, altKey: true }))).toBe("Ctrl+Alt+KeyM");
    expect(acceleratorFromEvent(key("ControlLeft", { ctrlKey: true }))).toBeNull();
    expect(acceleratorFromEvent(key("KeyM"))).toBeNull(); // needs a modifier
    expect(acceleratorFromEvent(key("F9"))).toBe("F9");
    expect(formatAccelerator("Ctrl+Alt+KeyM")).toBe("Ctrl + Alt + M");
    expect(formatAccelerator("Shift+Digit1")).toBe("Shift + 1");
  });
  it("finds duplicate bindings", () => {
    const d = duplicateAccelerators([
      { accelerator: "Ctrl+KeyA", action: "masterMute", targetId: "" },
      { accelerator: "ctrl+keya", action: "toggleMute", targetId: "mic" },
    ]);
    expect(d.has("ctrl+keya")).toBe(true);
  });
  it("maps actions to engine commands", () => {
    const hk = (action: string, targetId: string) => ({ accelerator: "", action, targetId }) as never;
    expect(hotkeyCommand(state, hk("toggleMute", "mic"), true)).toEqual({ cmd: "updateChannel", args: { id: "mic", mute: true } });
    expect(hotkeyCommand(state, hk("toggleMute", "main"), true)).toEqual({ cmd: "updateBus", args: { id: "main", mute: false } });
    expect(hotkeyCommand(state, hk("toggleMute", "mic"), false)).toBeNull();
    expect(hotkeyCommand(state, hk("pushToMute", "mic"), true)?.args.mute).toBe(true);
    expect(hotkeyCommand(state, hk("pushToMute", "mic"), false)?.args.mute).toBe(false);
    expect(hotkeyCommand(state, hk("volumeUp", "mic"), true)?.args.volume).toBeCloseTo(1.025, 3);
    expect(hotkeyCommand(state, hk("masterMute", ""), true)).toEqual({ cmd: "setMasterMute", args: { mute: true } });
    expect(hotkeyCommand(state, hk("applyScene", "s1"), true)?.cmd).toBe("applyScene");
    expect(hotkeyCommand(state, hk("loadProfile", "p1"), true)).toBeNull(); // already active
    expect(hotkeyCommand(state, hk("loadProfile", "p2"), true)?.cmd).toBe("loadProfile");
    expect(hotkeyCommand(state, hk("toggleMute", "gone"), true)).toBeNull();
  });
});

describe("device toasts", () => {
  const mk = (bus: string, ch: string) =>
    ({
      activeProfileId: "p",
      profile: {
        buses: [{ id: "b1", name: "Gaming Mic", deviceName: "CABLE Input" }],
        channels: [{ id: "c1", name: "Mic", type: "hardwareInput", deviceName: "USB Mic" }],
      },
      runtime: { buses: { b1: { status: bus, activeDeviceName: "CABLE Input" } }, channels: { c1: { status: ch } } },
    }) as unknown as EngineState;
  it("reports devices that go away or come back", () => {
    expect(deviceChanges(mk("connected", "connected"), mk("disconnected", "connected"))).toEqual([{ kind: "warn", text: "CABLE Input disconnected (Gaming Mic)" }]);
    expect(deviceChanges(mk("connected", "disconnected"), mk("connected", "connected"))).toEqual([{ kind: "info", text: "USB Mic reconnected" }]);
    expect(deviceChanges(mk("connected", "connected"), mk("connected", "connected"))).toEqual([]);
    expect(deviceChanges(null, mk("disconnected", "connected"))).toEqual([]);
  });
});
