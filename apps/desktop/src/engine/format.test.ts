import { describe, expect, it } from "vitest";
import { FADER_MAX, dbToFader, faderLabel, faderToGain, formatDb, gainToDb, meterPosition, parseDb, parsePercent, stepFaderDb } from "./format";

describe("format", () => {
  it("matches the engine fader taper", () => {
    expect(faderToGain(1)).toBe(1);
    expect(faderToGain(0)).toBe(0);
    expect(gainToDb(faderToGain(0.5))).toBeCloseTo(-12.04, 1);
    expect(gainToDb(faderToGain(FADER_MAX))).toBeCloseTo(20, 5);
    expect(gainToDb(faderToGain(1.25))).toBeCloseTo(10, 5);
    expect(faderLabel(FADER_MAX, true)).toBe("+20.0 dB");
  });
  it("labels managed faders in dB and session faders in percent", () => {
    expect(faderLabel(1, true)).toBe("0.0 dB");
    expect(faderLabel(0, true)).toBe("−∞ dB");
    expect(faderLabel(0.42, false)).toBe("42%");
    expect(formatDb(3.25)).toBe("+3.3 dB");
  });
  it("maps meter peaks onto a -60..+6 dBFS scale", () => {
    expect(meterPosition(0)).toBe(0);
    expect(meterPosition(1)).toBeCloseTo(60 / 66, 3);
    expect(meterPosition(10)).toBe(1);
  });
  it("converts dB back to fader positions", () => {
    for (const db of [-40, -12, -6, 0, 3.5, 10, 20]) expect(gainToDb(faderToGain(dbToFader(db)))).toBeCloseTo(db, 6);
    expect(dbToFader(-Infinity)).toBe(0);
  });
  it("steps faders in dB", () => {
    expect(gainToDb(faderToGain(stepFaderDb(1, 1, FADER_MAX)))).toBeCloseTo(1, 6);
    expect(gainToDb(faderToGain(stepFaderDb(1, -0.1, FADER_MAX)))).toBeCloseTo(-0.1, 6);
    expect(stepFaderDb(FADER_MAX, 3, FADER_MAX)).toBe(FADER_MAX);
    expect(stepFaderDb(1, 1, 1)).toBe(1); // app sessions stop at 0 dB
    expect(stepFaderDb(dbToFader(-60), -1, FADER_MAX)).toBe(0);
    expect(gainToDb(faderToGain(stepFaderDb(0, 1, FADER_MAX)))).toBeCloseTo(-60, 6);
    expect(stepFaderDb(0, -1, FADER_MAX)).toBe(0);
  });
  it("parses typed levels", () => {
    expect(parseDb("-6")).toBe(-6);
    expect(parseDb("+3.5")).toBe(3.5);
    expect(parseDb("0")).toBe(0);
    expect(parseDb(" 6 dB ")).toBe(6);
    expect(parseDb("−12")).toBe(-12);
    expect(parseDb("-inf")).toBe(-Infinity);
    expect(parseDb("−∞")).toBe(-Infinity);
    expect(parseDb("loud")).toBeNull();
    expect(parseDb("")).toBeNull();
    expect(parsePercent("50%")).toBe(50);
    expect(parsePercent("abc")).toBeNull();
  });
});
