import { describe, expect, it } from "vitest";

import { mapTelemetry } from "./telemetry.js";

describe("mapTelemetry", () => {
  it("maps real Raspberry Pi telemetry fields", () => {
    const result = mapTelemetry({
      telemetry: {
        attitude: { roll: 1.2, pitch: -2.3, yaw: 278.4 },
        pressure: { temperature: 26.5, press_abs: 1008.2 },
        motor: { pwm_us: [1100, 1200, 1300, 1400] },
        lora: { loss_rate_percent: 2.5 },
        battery: {
          battery_remaining: 76,
          voltage_battery: 11840,
          voltages: [11700]
        }
      }
    });

    expect(result).toEqual({
      attitude: { roll: 1.2, pitch: -2.3, yaw: 278.4 },
      environment: { temperature: 26.5, pressure: 1008.2, altitude: null },
      link: { rssi: null, packetLoss: 2.5, latency: null },
      motors: { pwm: [1100, 1200, 1300, 1400] },
      battery: { remaining: 76, voltage: 11840 }
    });
  });

  it("falls back to simulator telemetry fields", () => {
    const result = mapTelemetry({
      attitude: { roll_deg: 3, pitch_deg: 4, yaw_deg: 5 },
      environment: { temperature_c: 24, pressure_hpa: 1009, altitude_m: 12 },
      link: { rssi_dbm: -52, packet_loss_pct: 1.5, latency_ms: 22 },
      motors: { pwm: [1000, 1001, 1002, 1003] }
    });

    expect(result.attitude).toEqual({ roll: 3, pitch: 4, yaw: 5 });
    expect(result.environment).toEqual({ temperature: 24, pressure: 1009, altitude: 12 });
    expect(result.link).toEqual({ rssi: -52, packetLoss: 1.5, latency: 22 });
    expect(result.motors.pwm).toEqual([1000, 1001, 1002, 1003]);
  });

  it("returns null values instead of inventing missing telemetry", () => {
    const result = mapTelemetry({ telemetry: { battery: { voltages: [12100] } } });

    expect(result.attitude).toEqual({ roll: null, pitch: null, yaw: null });
    expect(result.environment).toEqual({ temperature: null, pressure: null, altitude: null });
    expect(result.link).toEqual({ rssi: null, packetLoss: null, latency: null });
    expect(result.motors.pwm).toEqual([null, null, null, null]);
    expect(result.battery).toEqual({ remaining: null, voltage: 12100 });
  });
});
