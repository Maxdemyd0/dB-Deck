import { useEffect, useRef } from "react";
import { meterPosition } from "../engine/format";
import { meters, onMeters } from "../engine/store";

/**
 * Peak meter fed by real engine measurements (engine PCM meters for managed
 * audio, Windows session peak meters for session-controlled apps). When no
 * measurement arrives it shows nothing — it never animates on its own.
 */
export function Meter({ kind, id, horizontal = false, stereo = true }: { kind: "channels" | "buses"; id: string; horizontal?: boolean; stereo?: boolean }) {
  const root = useRef<HTMLDivElement>(null);
  const hold = useRef([0, 0]);
  const holdAt = useRef([0, 0]);

  useEffect(() => {
    const update = () => {
      const el = root.current;
      if (!el) return;
      const m = meters[kind]?.[id];
      const bars = el.querySelectorAll<HTMLDivElement>(".bar");
      const values = m ? [m.peakL, m.peakR] : [0, 0];
      const now = performance.now();
      bars.forEach((bar, i) => {
        const p = meterPosition(values[i] ?? 0);
        const lvl = bar.firstElementChild as HTMLDivElement;
        const h = bar.lastElementChild as HTMLDivElement;
        if (horizontal) lvl.style.width = `${p * 100}%`;
        else lvl.style.height = `${p * 100}%`;
        if (p >= hold.current[i] || now - holdAt.current[i] > 1200) {
          hold.current[i] = p;
          holdAt.current[i] = now;
        }
        if (!horizontal) h.style.bottom = `calc(${hold.current[i] * 100}% - 2px)`;
        else h.style.display = "none";
      });
      el.classList.toggle("clip", !!m?.clip);
      el.classList.toggle("none", !m);
    };
    update();
    return onMeters(update);
  }, [kind, id, horizontal]);

  return (
    <div ref={root} className={`meter${horizontal ? " h" : ""}`} aria-hidden="true">
      <div className="bar">
        <div className="lvl" />
        <div className="hold" />
      </div>
      {stereo && (
        <div className="bar">
          <div className="lvl" />
          <div className="hold" />
        </div>
      )}
    </div>
  );
}
