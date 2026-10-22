// Display helpers. Kept pure for unit testing.

/** Top of a managed fader (+20 dB); 1.0 is unity. Mirrors engine/core/audio/Gain.h. */
export const FADER_MAX = 1.5;
export const FADER_MAX_BOOST_DB = 20;

/** Fader value (0..FADER_MAX) to engine gain, matching engine/core/audio/Gain.h. */
export function faderToGain(p: number): number {
  const c = Math.min(FADER_MAX, Math.max(0, p));
  if (c <= 1) return c * c;
  return Math.pow(10, (((c - 1) / (FADER_MAX - 1)) * FADER_MAX_BOOST_DB) / 20);
}

export function gainToDb(g: number): number {
  return g <= 1e-8 ? -Infinity : 20 * Math.log10(g);
}

/** Inverse of faderToGain for a level in dB (−∞ → 0). Not clamped to a fader's max. */
export function dbToFader(db: number): number {
  if (!isFinite(db)) return db > 0 ? FADER_MAX : 0;
  if (db <= 0) return Math.sqrt(Math.pow(10, db / 20));
  return 1 + (db / FADER_MAX_BOOST_DB) * (FADER_MAX - 1);
}

/** Lowest finite level a dB step lands on; one more step down is silence. */
export const STEP_FLOOR_DB = -60;

/** Moves a fader position by deltaDb, clamped to 0..max. */
export function stepFaderDb(position: number, deltaDb: number, max: number): number {
  const db = gainToDb(faderToGain(position));
  if (!isFinite(db)) return deltaDb > 0 ? Math.min(max, dbToFader(STEP_FLOOR_DB)) : 0;
  const next = Math.round((db + deltaDb) * 10) / 10;
  if (next < STEP_FLOOR_DB) return 0;
  return clamp(dbToFader(next), 0, max);
}

/**
 * Parses a typed level: "-6", "+3.5", "0", "6 dB", "-inf", "−∞".
 * Returns null when the text is not a level.
 */
export function parseDb(text: string): number | null {
  const t = text.trim().toLowerCase().replace(/−/g, "-").replace(/db$/, "").trim();
  if (/^-?(inf|infinity|∞)$/.test(t)) return -Infinity;
  if (!/^[+-]?(\d+\.?\d*|\.\d+)$/.test(t)) return null;
  return Number(t);
}

/** Parses a typed percentage ("50", "50%"); null when invalid. */
export function parsePercent(text: string): number | null {
  const t = text.trim().replace(/%$/, "").trim();
  if (!/^(\d+\.?\d*|\.\d+)$/.test(t)) return null;
  return Number(t);
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
