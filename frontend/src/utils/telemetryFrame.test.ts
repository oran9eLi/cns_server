import { describe, expect, it } from "vitest";

import {
  buildTelemetryFrameGroups,
  matchesTelemetryEntry,
  summarizeTelemetryFrame
} from "./telemetryFrame.js";

describe("telemetryFrame", () => {
  const frame = {
    schema_version: 3,
    device_id: "CNS0001",
    device_type: "cns_box",
    sent_at: "2026-08-03T10:00:00Z",
    telemetry: {
      attitude: { roll: 1.2, pitch: -2.3, healthy: true },
      motor: { pwm_us: [1100, 1200, 1300, 1400] },
      custom_module: { unknown_value: 42, note: null }
    },
    drone_id: { basic_id: { id_type: 1, ua_type: 2 } }
  };

  it("flattens every known and unknown leaf in a complete frame", () => {
    const groups = buildTelemetryFrameGroups(frame);
    const paths = groups.flatMap((group) => group.entries.map((entry) => entry.path));

    expect(paths).toEqual([
      "schema_version",
      "device_id",
      "device_type",
      "sent_at",
      "telemetry.attitude.roll",
      "telemetry.attitude.pitch",
      "telemetry.attitude.healthy",
      "telemetry.motor.pwm_us[0]",
      "telemetry.motor.pwm_us[1]",
      "telemetry.motor.pwm_us[2]",
      "telemetry.motor.pwm_us[3]",
      "telemetry.custom_module.unknown_value",
      "telemetry.custom_module.note",
      "drone_id.basic_id.id_type",
      "drone_id.basic_id.ua_type"
    ]);
    expect(paths.some((path) => path.includes("vendor_id"))).toBe(false);
  });

  it("preserves empty arrays and objects so the frame remains complete", () => {
    const groups = buildTelemetryFrameGroups({ telemetry: { empty_array: [], empty_object: {} } });
    const entries = groups.flatMap((group) => group.entries);

    expect(entries.map((entry) => [entry.path, entry.displayValue, entry.type])).toEqual([
      ["telemetry.empty_array", "[]", "empty"],
      ["telemetry.empty_object", "{}", "empty"]
    ]);
  });

  it("summarizes fields and searches by path, label, value, or unit", () => {
    const groups = buildTelemetryFrameGroups(frame);
    const summary = summarizeTelemetryFrame(frame, groups);
    const pwmEntry = groups.flatMap((group) => group.entries)
      .find((entry) => entry.path === "telemetry.motor.pwm_us[0]");

    expect(summary).toMatchObject({ fieldCount: 15, populatedCount: 14, groupCount: 8 });
    expect(summary.byteCount).toBeGreaterThan(0);
    expect(pwmEntry?.unit).toBe("μs");
    expect(pwmEntry && matchesTelemetryEntry(pwmEntry, "PWM")).toBe(true);
    expect(pwmEntry && matchesTelemetryEntry(pwmEntry, "1100")).toBe(true);
  });
});
