import { useEffect, useRef, useState } from "react";
import { clamp } from "../engine/format";

interface Props {
  /** Confirmed engine value, 0..1. */
  value: number;
  /** Called while dragging (throttled) and on release. Must send to the engine. */
  onChange: (v: number) => void;
  disabled?: boolean;
  muted?: boolean;
  label: string;
  /** Position of the unity mark (managed audio: 1.0 = 0 dB). */
  unity?: number;
  resetValue?: number;
  title?: string;
}

/**
 * Vertical fader. While the pointer is down the cap follows the pointer
 * locally; the moment it is released the fader shows the engine's confirmed
 * value again (which normally equals the last value sent).
 */
export function Fader({ value, onChange, disabled, muted, label, unity, resetValue = 1, title }: Props) {
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
    return clamp(1 - (clientY - r.top - 10) / (r.height - 20), 0, 1);
  };

  const shown = drag ?? value;
  const pct = shown * 100;

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
      onWheel={(e) => {
        if (disabled) return;
        send(clamp(value + (e.deltaY < 0 ? 0.02 : -0.02), 0, 1), true);
      }}
      onKeyDown={(e) => {
        if (disabled) return;
        const step = e.shiftKey ? 0.1 : 0.02;
        if (e.key === "ArrowUp" || e.key === "ArrowRight") send(clamp(value + step, 0, 1), true);
        else if (e.key === "ArrowDown" || e.key === "ArrowLeft") send(clamp(value - step, 0, 1), true);
        else if (e.key === "Home") send(1, true);
        else if (e.key === "End") send(0, true);
        else return;
        e.preventDefault();
      }}
    >
      <div className="track" />
      <div className="fill" style={{ height: `calc((100% - 20px) * ${shown})` }} />
      {unity !== undefined && <div className="unity" style={{ bottom: `calc(10px + (100% - 20px) * ${unity})` }} />}
      <div className="cap" style={{ bottom: `calc(10px + (100% - 20px) * ${shown})` }} />
    </div>
  );
}
