import Fastify from "fastify";
import { describe, expect, it } from "vitest";

import {
  CommandAcceptedResponseSchema,
  DeviceDetailResponseSchema,
  DeviceListResponseSchema,
  type BackendWebSocketEvent
} from "@cns/backend-protocol";

import { registerDeviceRoutes } from "../src/devices/deviceRoutes.js";
import { createCommandTracker } from "../src/commands/commandTracker.js";
import { createInMemoryDeviceStore } from "../src/devices/deviceStore.js";
import { createSeedDevices } from "../src/devices/seedDevices.js";
import type { WebSocketHub } from "../src/realtime/webSocketHub.js";
import type { RouteServiceGateway } from "../src/route/routeServiceGateway.js";

describe("设备接口", () => {
  it("返回符合协议的设备列表和详情", async () => {
    const app = await buildDeviceTestServer();

    const listResponse = await app.inject({
      method: "GET",
      url: "/api/devices?status=online"
    });
    const listBody = listResponse.json();

    expect(listResponse.statusCode).toBe(200);
    expect(() => DeviceListResponseSchema.parse(listBody)).not.toThrow();
    expect(listBody.items.every((item: { status: string }) => item.status === "online")).toBe(true);

    const detailResponse = await app.inject({
      method: "GET",
      url: "/api/devices/CNS00000000000000001"
    });

    await app.close();

    expect(detailResponse.statusCode).toBe(200);
    expect(() => DeviceDetailResponseSchema.parse(detailResponse.json())).not.toThrow();
    expect(detailResponse.json().item.device_id).toBe("CNS00000000000000001");
    expect(detailResponse.json().item).not.toHaveProperty("vendor_id");
  });

  it("拒绝离线设备命令", async () => {
    const app = await buildDeviceTestServer();

    const response = await app.inject({
      method: "POST",
      url: "/api/devices/CNS00000000000000003/commands",
      payload: {
        session_id: "session_test",
        client_request_id: "00000000-0000-4000-8000-000000000001",
        type: "control",
        command: "takeoff",
        parameters: {}
      }
    });

    await app.close();

    expect(response.statusCode).toBe(409);
    expect(response.json().error.code).toBe("target_offline");
  });

  it("接受在线设备命令并推送命令状态事件", async () => {
    const events: BackendWebSocketEvent[] = [];
    const app = await buildDeviceTestServer({
      hasSession: () => true,
      sendToSession: (_sessionId, event) => events.push(event)
    });

    const response = await app.inject({
      method: "POST",
      url: "/api/devices/CNS00000000000000001/commands",
      payload: {
        session_id: "session_test",
        client_request_id: "00000000-0000-4000-8000-000000000002",
        type: "control",
        command: "set_motor_pwm",
        parameters: {
          pwm_us: [1000, 1010, 1020, 1030]
        }
      }
    });

    await app.close();

    expect(response.statusCode).toBe(202);
    expect(() => CommandAcceptedResponseSchema.parse(response.json())).not.toThrow();
    expect(events).toEqual([]);
  });

  it("展示 PX4 设备但拒绝主控箱私有控制命令", async () => {
    const app = await buildDeviceTestServer();
    const deviceId = "PX4RID123456789ABCDE";

    const detail = await app.inject({
      method: "GET",
      url: `/api/devices/${deviceId}`
    });
    const control = await app.inject({
      method: "POST",
      url: `/api/devices/${deviceId}/commands`,
      payload: {
        session_id: "session_test",
        client_request_id: "00000000-0000-4000-8000-000000000003",
        type: "control",
        command: "takeoff",
        parameters: {}
      }
    });

    await app.close();

    expect(detail.statusCode).toBe(200);
    expect(detail.json().item.device_type).toBe("flight_controller");
    expect(control.statusCode).toBe(409);
    expect(control.json().error.code).toBe("unsupported_device_type");
  });

  it("不再提供 PX4 专属 RTT 探测接口", async () => {
    const app = await buildDeviceTestServer();
    const deviceId = "PX4RID123456789ABCDE";

    const response = await app.inject({
      method: "POST",
      url: `/api/devices/${deviceId}/px4-latency-probes`,
      payload: {
        session_id: "session_test",
        probe_id: "00000000-0000-4000-8000-000000000004"
      }
    });

    await app.close();

    expect(response.statusCode).toBe(404);
  });
});

async function buildDeviceTestServer(
  overrides: Partial<WebSocketHub> = {},
  routeOverrides: Partial<RouteServiceGateway> = {}
) {
  const app = Fastify({ logger: false });
  const realtime: WebSocketHub = {
    register: async () => undefined,
    hasSession: () => true,
    sendToSession: () => undefined,
    broadcast: () => undefined,
    broadcastTelemetry: () => undefined,
    ...overrides
  };

  await registerDeviceRoutes(app, {
    commands: createCommandTracker(1000),
    devices: createInMemoryDeviceStore(createSeedDevices()),
    realtime,
    routeService: {
      start: async () => undefined,
      setRealtimeInterest: async () => undefined,
      publishCommand: async () => undefined,
      dependencyStatus: async () => "ready",
      close: async () => undefined,
      ...routeOverrides
    }
  });

  return app;
}
