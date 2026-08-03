import { describe, expect, it } from "vitest";

import {
  FrontendWebSocketMessageSchema,
  Px4LatencyAckEventSchema,
  Px4LatencyProbeMessageSchema,
  Px4RealtimeEventSchema,
  Px4RealtimeFrameSchema
} from "@cns/backend-protocol";

describe("PX4 realtime protocol", () => {
  const frame = {
    schema_version: 1,
    device_id: "PX4RID123456789ABCDE",
    sequence: 42,
    sent_at: "2026-07-30T07:00:00.123Z",
    telemetry: {
      attitude: { roll: 1.2, pitch: -2.3, yaw: 40.5 }
    }
  };

  it("accepts compact realtime frames and events", () => {
    expect(Px4RealtimeFrameSchema.parse(frame)).toEqual(frame);
    expect(Px4RealtimeEventSchema.parse({
      type: "px4.realtime",
      ...frame,
      server_received_at: "2026-07-30T07:00:00.140Z"
    }).sequence).toBe(42);
  });

  it("rejects invalid device subscriptions and extra fields", () => {
    expect(FrontendWebSocketMessageSchema.safeParse({
      type: "px4.subscribe",
      device_id: "bad/device"
    }).success).toBe(false);
    expect(Px4RealtimeFrameSchema.safeParse({
      ...frame,
      retained: true
    }).success).toBe(false);
  });

  it("accepts correlated latency probes and ACK events", () => {
    const probe = {
      schema_version: 1,
      device_id: "PX4RID123456789ABCDE",
      session_id: "session_test",
      probe_id: "00000000-0000-4000-8000-000000000001"
    };
    expect(Px4LatencyProbeMessageSchema.parse(probe)).toEqual(probe);
    expect(Px4LatencyAckEventSchema.parse({
      type: "px4.latency_ack",
      ...probe
    }).probe_id).toBe(probe.probe_id);
  });
});
