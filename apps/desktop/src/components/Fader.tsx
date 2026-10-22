import { useEffect, useRef, useState } from "react";
import { clamp, stepFaderDb } from "../engine/format";

interface Props {
  /** Confirmed engine value, 0..max. */
  value: number;
  /** Top of the fader travel (1 = unity only; 1.5 = boost to +20 dB). */
  max?: number;
  /** Called while dragging (throttled) and on release. Must send to the engine. */
  onChange: (v: number) => void;
  disabled?: boolean;
  muted?: boolean;
  label: string;
  /** Position of the unity mark (managed audio: 1.0 = 0 dB). */
  unity?: number;
  resetValue?: number;
  title?: string;
  /** "db": wheel/arrow keys step in dB (managed audio). "percent": Windows session volume. */
  scale?: "db" | "percent";
}

/**
 * Vertical fader. While the pointer is down the cap follows the pointer
 * locally; the moment it is released the fader shows the engine's confirmed
 * value again (which normally equals the last value sent).
 */
export function Fader({ value, onChange, disabled, muted, label, unity, resetValue = 1, title, max = 1, scale = "db" }: Props) {
  const ref = useRef<HTMLDivElement>(null);
  const [drag, setDrag] = useState<number | null>(null);
  const lastSent = useRef(0);
  const pending = useRef<number | null>(null);
  const timer = useRef<number | null>(null);

  const send = (v: number, force = false) => {
    const now = performance.now();
    pending.current = v;
    if (force || now - lastSent.current > 33) {
      lastSent.current = now;
      pending.current = null;
      onChange(v);
    } else if (timer.current === null) {
      timer.current = window.setTimeout(() => {
        timer.current = null;
        if (pending.current !== null) {
          lastSent.current = performance.now();
          onChange(pending.current);
          pending.current = null;
        }
      }, 34);
    }
  };
  useEffect(() => () => void (timer.current && clearTimeout(timer.current)), []);

  const posFromEvent = (clientY: number) => {
    const r = ref.current!.getBoundingClientRect();
    return clamp(1 - (clientY - r.top - 10) / (r.height - 20), 0, 1) * max;
  };

  // One step: 1 dB (Ctrl 0.1 dB, Shift 3 dB), or 2% (Ctrl 1%, Shift 10%) for session volume.
  const step = (dir: 1 | -1, e: { ctrlKey: boolean; shiftKey: boolean }) => {
    if (scale === "db") return stepFaderDb(value, dir * (e.ctrlKey ? 0.1 : e.shiftKey ? 3 : 1), max);
    return clamp(value + dir * (e.ctrlKey ? 0.01 : e.shiftKey ? 0.1 : 0.02), 0, max);
  };

  // Wheel needs a non-passive listener so Ctrl+wheel adjusts finely instead of zooming the page.
  const onWheel = useRef<(e: WheelEvent) => void>(() => {});
  onWheel.current = (e) => {
    if (disabled || e.deltaY === 0) return;
    e.preventDefault();
    send(step(e.deltaY < 0 ? 1 : -1, e), true);
  };
  useEffect(() => {
    const el = ref.current!;
    const h = (e: WheelEvent) => onWheel.current(e);
    el.addEventListener("wheel", h, { passive: false });
    return () => el.removeEventListener("wheel", h);
  }, []);

  const shown = drag ?? value;
  const pos = clamp(shown / max, 0, 1);  // 0..1 along the track
  const pct = pos * 100;

  return (
    <div
      ref={ref}
      className={`fader${disabled ? " disabled" : ""}${muted ? " muted" : ""}`}
      role="slider"
      aria-label={label}
      aria-valuemin={0}
      aria-valuemax={100}
      aria-valuenow={Math.round(pct)}
      aria-disabled={disabled}
      tabIndex={disabled ? -1 : 0}
      title={title}
      onPointerDown={(e) => {
        if (disabled || e.button !== 0) return;
        e.currentTarget.setPointerCapture(e.pointerId);
        const v = posFromEvent(e.clientY);
        setDrag(v);
        send(v);
      }}
      onPointerMove={(e) => {
        if (drag === null) return;
        const v = posFromEvent(e.clientY);
        setDrag(v);
        send(v);
      }}
      onPointerUp={(e) => {
        if (drag === null) return;
        const v = posFromEvent(e.clientY);
        send(v, true);
        setDrag(null);
      }}
      onPointerCancel={() => setDrag(null)}
      onDoubleClick={() => !disabled && send(resetValue, true)}
      onKeyDown={(e) => {
        if (disabled) return;
        if (e.key === "ArrowUp" || e.key === "ArrowRight") send(step(1, e), true);
        else if (e.key === "ArrowDown" || e.key === "ArrowLeft") send(step(-1, e), true);
        else if (e.key === "Home") send(Math.min(1, max), true);
        else if (e.key === "End") send(0, true);
        else return;
        e.preventDefault();
      }}
    >
      <div className="track" />
      <div className="fill" style={{ height: `calc((100% - 20px) * ${pos})` }} />
      {unity !== undefined && <div className="unity" title="0 dB" style={{ bottom: `calc(10px + (100% - 20px) * ${unity / max})` }} />}
      <div className="cap" style={{ bottom: `calc(10px + (100% - 20px) * ${pos})` }} />
    </div>
  );
}
