import { describe, expect, it } from "vitest";
import { FADER_MAX, faderLabel, faderToGain, formatDb, gainToDb, meterPosition } from "./format";

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
});
