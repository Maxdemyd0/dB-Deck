// Display helpers. Kept pure for unit testing.

/** Fader position (0..1) to engine gain, matching engine/core/audio/Gain.h. */
export function faderToGain(p: number): number {
  const c = Math.min(1, Math.max(0, p));
  return c * c;
}

export function gainToDb(g: number): number {
  return g <= 1e-8 ? -Infinity : 20 * Math.log10(g);
}

export function formatDb(db: number, digits = 1): string {
  if (!isFinite(db)) return "−∞ dB";
  const s = db.toFixed(digits);
  return `${db > 0 ? "+" : db < 0 ? "−" : ""}${s.replace("-", "")} dB`;
}

/** Label for a fader: managed audio shows dB, Windows sessions show percent. */
export function faderLabel(position: number, managed: boolean): string {
  if (!managed) return `${Math.round(position * 100)}%`;
  return formatDb(gainToDb(faderToGain(position)));
}

/** Meter position (0..1) for a linear peak on a -60..+6 dBFS scale. */
export function meterPosition(peak: number): number {
  if (peak <= 0) return 0;
  const db = 20 * Math.log10(peak);
  return Math.min(1, Math.max(0, (db + 60) / 66));
}

export function clamp(v: number, lo: number, hi: number): number {
  return Math.min(hi, Math.max(lo, v));
}
