import { describe, expect, it } from "vitest";

import {
  BackendWebSocketEventSchema,
  DeviceRealtimeEventSchema,
  DeviceRealtimeFrameSchema,
  FrontendWebSocketMessageSchema
} from "@cns/backend-protocol";

describe("通用设备实时遥测协议", () => {
  const motorFrame = {
    schema_version: 1,
    device_id: "CNS00000000000000001",
    sequence: 42,
    sent_at: "2026-08-05T01:02:03.123Z",
    telemetry: { motor: { pwm_us: [1000, 1010, 1020, 1030] } }
  };

  it("接受主控箱和PX4共用的实时外壳", () => {
    expect(DeviceRealtimeFrameSchema.parse(motorFrame)).toEqual(motorFrame);
    expect(DeviceRealtimeEventSchema.parse({
      type: "telemetry.realtime",
      ...motorFrame,
      server_received_at: "2026-08-05T01:02:03.140Z"
    }).telemetry).toEqual(motorFrame.telemetry);
  });

  it("拒绝额外字段和非v1帧", () => {
    expect(DeviceRealtimeFrameSchema.safeParse({
      ...motorFrame,
      retained: true
    }).success).toBe(false);
    expect(DeviceRealtimeFrameSchema.safeParse({
      ...motorFrame,
      schema_version: 3
    }).success).toBe(false);
  });

  it("只接受新的通用订阅和事件消息", () => {
    expect(FrontendWebSocketMessageSchema.safeParse({
      type: "telemetry.subscribe",
      device_id: motorFrame.device_id
    }).success).toBe(true);
    expect(FrontendWebSocketMessageSchema.safeParse({
      type: "px4.subscribe",
      device_id: motorFrame.device_id
    }).success).toBe(false);
    expect(BackendWebSocketEventSchema.safeParse({
      type: "px4.latency_ack",
      schema_version: 1,
      device_id: motorFrame.device_id,
      session_id: "session_test",
      probe_id: "00000000-0000-4000-8000-000000000001"
    }).success).toBe(false);
  });
});
