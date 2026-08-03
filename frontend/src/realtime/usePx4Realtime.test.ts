import { describe, expect, it } from "vitest";

import { mergePx4Telemetry } from "./usePx4Realtime.js";

describe("mergePx4Telemetry", () => {
  it("keeps slow identity and 5G fields while replacing fast flight fields", () => {
    const merged = mergePx4Telemetry(
      {
        identity: { remote_id: "RID-001" },
        telemetry: {
          attitude: { roll: 1 },
          cellular_5g: { latency_ms: 35 }
        }
      },
      {
        type: "px4.realtime",
        schema_version: 1,
        device_id: "PX4RID123456789ABCDE",
        sequence: 9,
        sent_at: "2026-07-30T07:00:00.123Z",
        server_received_at: "2026-07-30T07:00:00.140Z",
        telemetry: {
          attitude: { roll: 8, pitch: 2 }
        }
      }
    );

    expect(merged).toMatchObject({
      identity: { remote_id: "RID-001" },
      telemetry: {
        attitude: { roll: 8, pitch: 2 },
        cellular_5g: { latency_ms: 35 }
      }
    });
  });
});
