import { useState } from "react";
import { clamp, dbToFader, faderLabel, parseDb, parsePercent } from "../engine/format";

interface Props {
  /** Fader position, 0..max. */
  value: number;
  max: number;
  /** "db" for managed audio, "percent" for Windows session volume. */
  scale: "db" | "percent";
  onCommit: (v: number) => void;
  title?: string;
  disabled?: boolean;
}

/** The level under a fader. Click it to type an exact value (-6, +3.5, -inf, or 50%). */
export function StripValue({ value, max, scale, onCommit, title, disabled }: Props) {
  const [text, setText] = useState<string | null>(null);
  const label = faderLabel(value, scale === "db");

  const commit = () => {
    if (text === null) return;
    const t = text;
    setText(null);
    if (scale === "db") {
      const db = parseDb(t);
      if (db !== null) onCommit(clamp(dbToFader(db), 0, max));
    } else {
      const pct = parsePercent(t);
      if (pct !== null) onCommit(clamp(pct / 100, 0, max));
    }
  };

  if (text !== null)
    return (
      <input
        className="strip-value editing"
        autoFocus
        value={text}
        aria-label={scale === "db" ? "Level in dB" : "Volume in percent"}
        onFocus={(e) => e.currentTarget.select()}
        onChange={(e) => setText(e.target.value)}
        onBlur={commit}
        onKeyDown={(e) => {
          if (e.key === "Enter") commit();
          if (e.key === "Escape") setText(null);
        }}
      />
    );

  return (
    <button
      className="strip-value"
      disabled={disabled}
      title={`${title ? title + "\n" : ""}Click to type a level`}
      onClick={() => setText(scale === "db" ? label.replace(/ dB$/, "").replace("−", "-").replace("∞", "inf") : String(Math.round(value * 100)))}
    >
      {label}
    </button>
  );
}
