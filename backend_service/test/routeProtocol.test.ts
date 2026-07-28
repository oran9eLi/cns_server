import { describe, expect, it } from "vitest";

import {
  RouteCommandAckSchema,
  RouteDeviceStateMessageSchema,
  RouteOnlineDeviceSnapshotSchema,
  buildRouteCommandRequest,
  isTerminalRouteStatus,
  toCommandUpdatedEvent,
  toDeviceStateEvent
} from "../src/route/routeProtocol.js";

describe("route_service protocol adapter", () => {
  it("builds the exact control request expected by route_service", () => {
    expect(buildRouteCommandRequest("CNS00000000000000001", {
      session_id: "session_test",
      client_request_id: "00000000-0000-4000-8000-000000000001",
      type: "control",
      command: "set_motor_pwm",
      parameters: { pwm_us: [1000, 1100, 1200, 1300] }
    })).toEqual({
      schema_version: 1,
      request_id: "00000000-0000-4000-8000-000000000001",
      target: { vendor_id: "CNS00000000000000001" },
      command: "set_motor_pwm",
      parameters: { pwm_us: [1000, 1100, 1200, 1300] }
    });
  });

  it("maps route_service state events to browser events", () => {
    const message = RouteDeviceStateMessageSchema.parse({
      schema_version: 1,
      event_type: "device_state",
      event_at: "2026-07-21T05:00:00.000Z",
      revision: 12,
      vendor_id: "CNS00000000000000001",
      school_id: 7,
      school_name: "东创航空实训中心",
      dcdw_label: "DCDW-001",
      model_version: "CNS v1.0",
      status: "online",
      last_seen_at: "2026-07-21T05:00:00.000Z",
      telemetry_received_at: "2026-07-21T05:00:00.000Z",
      latest_telemetry: { attitude: { roll_deg: 1.2 } },
      change_reason: "telemetry",
      degraded: false
    });

    expect(toDeviceStateEvent(message)).toMatchObject({
      type: "device.state",
      event_at: "2026-07-21T05:00:00.000Z",
      school_name: "东创航空实训中心",
      change_reason: "telemetry"
    });
  });

  it("accepts the retained online device snapshot contract", () => {
    expect(RouteOnlineDeviceSnapshotSchema.parse({
      schema_version: 1,
      event_type: "online_device_snapshot",
      generated_at: "2026-07-27T05:00:00.000Z",
      revision: 3,
      device_ids: [
        "CNS00000000000000001",
        "CNS00000000000000002"
      ]
    }).device_ids).toHaveLength(2);
  });

  it("maps pending ACK to submitted and preserves terminal ACK details", () => {
    const ack = RouteCommandAckSchema.parse({
      schema_version: 1,
      request_id: "00000000-0000-4000-8000-000000000001",
      command_id: "10000000-0000-4000-8000-000000000001",
      command_type: "control",
      status: "pending",
      business_status: null,
      occurred_at: "2026-07-21T05:00:00.000Z"
    });

    expect(toCommandUpdatedEvent(ack, {
      sessionId: "session_test",
      vendorId: "CNS00000000000000001",
      commandType: "control",
      command: "takeoff"
    })).toMatchObject({
      status: "submitted",
      command_id: "10000000-0000-4000-8000-000000000001"
    });
    expect(isTerminalRouteStatus("succeeded")).toBe(true);
    expect(isTerminalRouteStatus("in_progress")).toBe(false);
  });
});
