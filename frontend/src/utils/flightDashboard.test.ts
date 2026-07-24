import { describe, expect, it } from "vitest";

import { mapFlightDashboard, readFlightValue } from "./flightDashboard.js";

describe("mapFlightDashboard", () => {
  const completeFrame = {
    self_check: {
      position: "offline",
      attitude: true,
      environment: { status: "normal" },
      lora: "warning",
      cellular_5g: false,
      remote_id: "ready",
      storage: "normal"
    },
    alerts: [
      { code: "0x0301", level: "warning", message: "5G 通信离线", occurred_at: "2026-07-22T08:00:00+08:00" }
    ],
    logs: [
      { level: "info", message: "姿态正常", time: "2026-07-22T08:01:00+08:00" }
    ],
    telemetry: {
      pressure: { altitude: 18.6, humidity: 57.9 },
      battery: {
        voltages: [11600, 11300],
        battery_remaining: [45, 60]
      },
      motor: { pwm_us: [1000, 1000, 1000, 1000] }
    }
  };

  it("maps height, humidity, battery 1 as controller and battery 2 as motor", () => {
    const result = mapFlightDashboard(completeFrame);

    expect(result.environment).toEqual({ altitude: 18.6, humidity: 57.9 });
    expect(result.power.controller).toEqual({ voltage: 11600, remaining: 45 });
    expect(result.power.motor).toEqual({ voltage: 11300, remaining: 60 });
  });

  it("maps explicit self-check states, alerts, and logs", () => {
    const result = mapFlightDashboard(completeFrame);

    expect(result.modules.find((item) => item.key === "position")?.status).toBe("error");
    expect(result.modules.find((item) => item.key === "attitude")?.status).toBe("normal");
    expect(result.modules.find((item) => item.key === "lora")?.status).toBe("error");
    expect(result.alerts[0]).toMatchObject({ code: "0x0301", message: "5G 通信离线" });
    expect(result.logs[0]).toMatchObject({ level: "info", message: "姿态正常" });
  });

  it("supports indexed paths and falls back to evidence without inventing normal status", () => {
    expect(readFlightValue(completeFrame, "telemetry.battery.voltages[1]")).toBe(11300);

    const result = mapFlightDashboard({ telemetry: { attitude: { roll: 1 } } });
    expect(result.modules.find((item) => item.key === "attitude")).toMatchObject({
      status: "error",
      detail: "异常"
    });
    expect(result.modules.find((item) => item.key === "storage")?.status).toBe("error");
  });

  it("does not reuse the controller battery scalar as the motor battery", () => {
    const result = mapFlightDashboard({
      telemetry: {
        battery: { voltages: [11700], voltage_battery: 11840, battery_remaining: 76 }
      }
    });

    expect(result.power.controller).toEqual({ voltage: 11840, remaining: 76 });
    expect(result.power.motor).toEqual({ voltage: null, remaining: null });
  });

  it("maps backend GCJ-02 coordinates while preserving WGS84", () => {
    const result = mapFlightDashboard({
      telemetry: {
        position: {
          latitude_wgs84: 39.915,
          longitude_wgs84: 116.404,
          latitude_gcj02: 39.9164042815,
          longitude_gcj02: 116.4102444992,
          heading_deg: 156.4
        }
      }
    });

    expect(result.position).toEqual({
      fixValid: null,
      fixType: null,
      latitudeWgs84: 39.915,
      longitudeWgs84: 116.404,
      latitudeGcj02: 39.9164042815,
      longitudeGcj02: 116.4102444992,
      heading: 156.4,
      altitude: null,
      satellites: null,
      horizontalAccuracy: null
    });
  });

  it("preserves an explicit invalid GPS fix", () => {
    const result = mapFlightDashboard({
      telemetry: {
        position: { latitude: 39.915, longitude: 116.404, fix_valid: false }
      }
    });

    expect(result.position.fixValid).toBe(false);
  });

  it("maps the cns_rpi GPS and global_position fields", () => {
    const result = mapFlightDashboard({
      telemetry: {
        gps: {
          lat: 312304160,
          lon: 1214737010,
          alt: 11.7,
          fix_type: 3,
          satellites_visible: 12,
          h_acc: 0.8
        },
        global_position: { lat: 312304160, lon: 1214737010, hdg: 207 }
      }
    });

    expect(result.position).toMatchObject({
      fixValid: true,
      fixType: 3,
      latitudeWgs84: 31.230416,
      longitudeWgs84: 121.473701,
      heading: 207,
      altitude: 11.7,
      satellites: 12,
      horizontalAccuracy: 0.8
    });
  });

  it("maps a complete real cns_rpi frame without field loss or battery crossover", () => {
    const result = mapFlightDashboard({
      telemetry: {
        gps: { lat: 31.230416, lon: 121.473701, alt: 11.7, fix_type: 3, satellites_visible: 12, h_acc: 0.8 },
        global_position: { lat: 31.230416, lon: 121.473701, alt: 11.7, hdg: 207 },
        sys_status: { voltage_battery: 11.6, battery_remaining: 79 },
        battery: { voltages: [3.87, 3.86, 3.87, null], battery_remaining: 78 },
        battery2: { voltages: [3.8, 3.8, 3.8, null], battery_remaining: 72 },
        humidity: { humidity_percent: 53.5 },
        cellular_5g: { rssi_dbm: -75, packet_loss_percent: 0.8, latency_ms: 42.7 },
        motor: { pwm_us: [1000, 1100, 1200, 1300] }
      },
      modules: [
        { name: "GNSS", status: "ONLINE" },
        { name: "IMU", status: "ONLINE" },
        { name: "BARO", status: "DEGRADED" },
        { name: "LORA", status: "OFFLINE" },
        { name: "5G", status: "ONLINE" },
        { name: "STORAGE", status: "ONLINE" },
        { name: "REMOTE_ID", status: "ONLINE" },
        { name: "CONTROL", status: "ONLINE" }
      ],
      logs: {
        latest_seq: 31,
        entries: [
          { sequence: 30, message_id: 30, time: "14:23:06", severity: 0 },
          { sequence: 31, message_id: 31, time: "14:23:07", severity: 2 }
        ]
      }
    });

    expect(result.environment).toEqual({ altitude: 11.7, humidity: 53.5 });
    expect(result.power.controller).toEqual({ voltage: 11.6, remaining: 78 });
    expect(result.power.motor.voltage).toBeCloseTo(11.4, 8);
    expect(result.power.motor.remaining).toBe(72);
    expect(result.modules.find((item) => item.key === "position")).toMatchObject({ status: "normal", detail: "正常" });
    expect(result.modules.find((item) => item.key === "environment")).toMatchObject({ status: "error", detail: "异常" });
    expect(result.modules.find((item) => item.key === "lora")).toMatchObject({ status: "error", detail: "异常" });
    expect(result.modules.find((item) => item.key === "motor")).toMatchObject({ status: "normal", detail: "正常" });
    expect(result.logs).toHaveLength(2);
    expect(result.logs[0]).toMatchObject({ message: "5G 正常", level: "info", occurredAt: "14:23:06" });
    expect(result.logs[1]).toMatchObject({ message: "5G 断开", level: "error", occurredAt: "14:23:07" });
  });

  it("only allows position and motor modules to use the yellow warning state", () => {
    const result = mapFlightDashboard({
      self_check: {
        position: "warning",
        attitude: "warning",
        environment: "starting",
        lora: "degraded",
        cellular_5g: "warning",
        remote_id: "warning",
        storage: "warning",
        motor: "warning"
      }
    });

    expect(result.modules.find((item) => item.key === "position")?.status).toBe("warning");
    expect(result.modules.find((item) => item.key === "motor")?.status).toBe("warning");
    for (const key of ["attitude", "environment", "lora", "cellular", "remote_id", "storage"]) {
      expect(result.modules.find((item) => item.key === key)?.status).toBe("error");
    }
  });

  it("colors disconnected and offline log messages red even without an error severity", () => {
    const result = mapFlightDashboard({
      logs: [
        { message_id: 12, time: "00:00:01" },
        { message: "5G 离线", severity: 0, time: "00:00:02" },
        { message_id: 30, severity: 2, time: "00:00:03" }
      ]
    });

    expect(result.logs.map((item) => item.level)).toEqual(["error", "error", "info"]);
  });
});
