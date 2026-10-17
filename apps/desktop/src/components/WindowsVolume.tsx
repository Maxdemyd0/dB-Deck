import { useEffect, useRef, useState } from "react";
import { request } from "../engine/store";
import type { EndpointVolume } from "../engine/types";
import { Icon } from "./Icon";

/**
 * Windows endpoint master volume (IAudioEndpointVolume in the engine). This is
 * the same control as Quick Settings and the keyboard volume keys; changes
 * made anywhere arrive from the engine as notifications. Separate from the
 * dB-Deck Main bus level.
 */
export function WindowsVolume({ ep, target, compact }: { ep: EndpointVolume; target: "default" | "main"; compact?: boolean }) {
  const [drag, setDrag] = useState<number | null>(null);
  const [flash, setFlash] = useState(false);
  const last = useRef(ep.changeCount);
  const timer = useRef<number | null>(null);

  // Brief highlight when the change came from outside dB-Deck.
  useEffect(() => {
    if (ep.changeCount !== last.current) {
      last.current = ep.changeCount;
      if (ep.lastChangeSource === "external") {
        setFlash(true);
        const t = setTimeout(() => setFlash(false), 500);
        return () => clearTimeout(t);
      }
    }
  }, [ep.changeCount, ep.lastChangeSource]);

  if (!ep.bound || !ep.available) {
    return (
      <div className="vol-widget" title={ep.unavailableReason || "No Windows playback device"}>
        <Icon name="windows" size={14} />
        <span className="faint small">{ep.bound ? "No software volume" : "No device"}</span>
      </div>
    );
  }
  const send = (v: number) => {
    if (timer.current) return;
    timer.current = window.setTimeout(() => (timer.current = null), 30);
    void request("setWindowsVolume", { target, volume: v }, true).catch(() => {});
  };
  const shown = drag ?? ep.volume;
  return (
    <div className={`vol-widget${flash ? " flash" : ""}`} title={`Windows volume — ${ep.deviceName}${ep.hardwareVolume ? " (hardware volume)" : ""}`}>
      <button
        className={`btn ghost icon small${ep.mute ? " mute on" : ""}`}
        onClick={() => request("setWindowsVolume", { target, mute: !ep.mute }).catch(() => {})}
        aria-pressed={ep.mute}
        aria-label={ep.mute ? "Unmute Windows volume" : "Mute Windows volume"}
      >
        <Icon name={ep.mute ? "mute" : "windows"} size={14} />
      </button>
      <input
        className="hslider"
        type="range"
        min={0}
        max={1}
        step={0.01}
        value={shown}
        aria-label={`Windows volume for ${ep.deviceName}`}
        onPointerDown={() => setDrag(ep.volume)}
        onPointerUp={(e) => {
          const v = Number((e.target as HTMLInputElement).value);
          void request("setWindowsVolume", { target, volume: v }).catch(() => {});
          setDrag(null);
        }}
        onChange={(e) => {
          const v = Number(e.target.value);
          if (drag !== null) setDrag(v);
          send(v);
        }}
      />
      <span className="pct">{Math.round(shown * 100)}%</span>
      {!compact && <span className="nm ellipsis">{ep.deviceName}</span>}
    </div>
  );
}
