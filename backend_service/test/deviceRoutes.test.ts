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
    expect(detailResponse.json().item.vendor_id).toBe("CNS00000000000000001");
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
    ...overrides
  };

  await registerDeviceRoutes(app, {
    commands: createCommandTracker(1000),
    devices: createInMemoryDeviceStore(createSeedDevices()),
    realtime,
    routeService: {
      start: async () => undefined,
      publishCommand: async () => undefined,
      dependencyStatus: async () => "ready",
      close: async () => undefined,
      ...routeOverrides
    }
  });

  return app;
}
