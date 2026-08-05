import { describe, expect, it } from "vitest";

import { mergeDeviceTelemetry } from "./useDeviceRealtime.js";

describe("mergeDeviceTelemetry", () => {
  it("主控箱motor实时字段覆盖快照但保留慢变字段", () => {
    const merged = mergeDeviceTelemetry(
      {
        identity: { remote_id: "RID-001" },
        telemetry: {
          motor: { pwm_us: [1000, 1000, 1000, 1000] },
          cellular_5g: { latency_ms: 35 }
        }
      },
      {
        type: "telemetry.realtime",
        schema_version: 1,
        device_id: "CNS00000000000000001",
        sequence: 9,
        sent_at: "2026-08-05T01:02:03.123Z",
        server_received_at: "2026-08-05T01:02:03.140Z",
        telemetry: { motor: { pwm_us: [1100, 1110, 1120, 1130] } }
      }
    );

    expect(merged).toMatchObject({
      identity: { remote_id: "RID-001" },
      telemetry: {
        motor: { pwm_us: [1100, 1110, 1120, 1130] },
        cellular_5g: { latency_ms: 35 }
      }
    });
  });

  it("PX4姿态使用同一个通用实时事件覆盖快照", () => {
    const merged = mergeDeviceTelemetry(
      {
        telemetry: {
          attitude: { roll: 1 },
          gps: { satellites_visible: 12 }
        }
      },
      {
        type: "telemetry.realtime",
        schema_version: 1,
        device_id: "PX4RID123456789ABCDE",
        sequence: 10,
        sent_at: "2026-08-05T01:02:04.123Z",
        server_received_at: "2026-08-05T01:02:04.140Z",
        telemetry: { attitude: { roll: 8, pitch: 2 } }
      }
    );

    expect(merged).toMatchObject({
      telemetry: {
        attitude: { roll: 8, pitch: 2 },
        gps: { satellites_visible: 12 }
      }
    });
  });
});
