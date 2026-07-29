import websocket from "@fastify/websocket";
import Fastify from "fastify";
import { randomUUID } from "node:crypto";

import {
  CommandUpdatedEventSchema,
  DeviceCommandRequestSchema,
  DeviceStateEventSchema,
  HealthResponseSchema,
  SCHEMA_VERSION,
  SessionReadyEventSchema,
  type BackendWebSocketEvent,
  type CommandStatus,
  type DeviceDetail,
  type DeviceListQuery
} from "@cns/backend-protocol";

import { cloneInitialDevices, nudgeTelemetry } from "./fixtures/devices.js";

type WebSocketConnection = {
  send?(payload: string): void;
  close?(): void;
  on?(event: "close", handler: () => void): void;
  socket?: WebSocketPeer;
};

type WebSocketPeer = {
  send(payload: string): void;
  close?(): void;
  on?(event: "close", handler: () => void): void;
};

const app = Fastify({ logger: false });
const sessions = new Map<string, WebSocketConnection>();
let devices = cloneInitialDevices();
let tick = 0;
let nextCommandResult: CommandStatus = "succeeded";

await app.register(websocket);

app.get("/api/health", async () =>
  HealthResponseSchema.parse({
    schema_version: SCHEMA_VERSION,
    status: "ok",
    server_time: new Date().toISOString(),
    dependencies: {
      database: "ready",
      mqtt: "ready"
    }
  })
);

app.get("/api/devices", async (request) => {
  const query = request.query as DeviceListQuery;
  let items = devices;

  if (query.keyword) {
    const keyword = query.keyword.toLowerCase();
    items = items.filter((device) =>
      [device.device_id, device.vendor_id, device.school_name ?? "",
        device.dcdw_label ?? ""].some((value) =>
        value.toLowerCase().includes(keyword)
      )
    );
  }
  if (query.school_name) {
    items = items.filter((device) => device.school_name === query.school_name);
  }
  if (query.status) {
    items = items.filter((device) => device.status === query.status);
  }

  return {
    schema_version: SCHEMA_VERSION,
    items: items.map(({ provisioned_at: _provisionedAt, latest_telemetry: _latestTelemetry, ...summary }) => summary)
  };
});

app.get("/api/devices/:vendor_id", async (request, reply) => {
  const { vendor_id } = request.params as { vendor_id: string };
  const item = devices.find((device) => device.vendor_id === vendor_id);
  if (!item) {
    return reply.code(404).send({
      error: {
        code: "not_found",
        message: "未找到设备"
      }
    });
  }

  return {
    schema_version: SCHEMA_VERSION,
    item
  };
});

app.post("/api/devices/:vendor_id/commands", async (request, reply) => {
  const { vendor_id } = request.params as { vendor_id: string };
  const item = devices.find((device) => device.vendor_id === vendor_id);
  if (!item) {
    return reply.code(404).send({ error: { code: "not_found", message: "未找到设备" } });
  }
  if (item.status !== "online") {
    return reply.code(409).send({ error: { code: "target_offline", message: "目标设备离线" } });
  }

  const parsed = DeviceCommandRequestSchema.safeParse(request.body);
  if (!parsed.success) {
    return reply.code(400).send({ error: { code: "invalid_parameter", message: "命令参数不符合协议" } });
  }
  if (item.device_type === "flight_controller" && parsed.data.type === "control") {
    return reply.code(409).send({
      error: {
        code: "unsupported_device_type",
        message: "真实飞控不支持主控箱私有控制命令"
      }
    });
  }

  const session = sessions.get(parsed.data.session_id);
  if (!session) {
    return reply.code(409).send({
      error: {
        code: "websocket_session_required",
        message: "当前 WebSocket 会话不可用，请刷新页面后重试"
      }
    });
  }

  const commandName = parsed.data.type === "control" ? parsed.data.command : null;
  const commandType = parsed.data.type;
  const commandId = randomUUID();
  const statuses: CommandStatus[] = ["submitted", "dispatched", "in_progress", nextCommandResult];
  nextCommandResult = "succeeded";

  statuses.forEach((status, index) => {
    setTimeout(() => {
      sendToSession(parsed.data.session_id, {
        type: "command.updated",
        schema_version: SCHEMA_VERSION,
        client_request_id: parsed.data.client_request_id,
        command_id: commandId,
        vendor_id,
        command_type: commandType,
        command: commandName,
        status,
        business_status: businessStatusFor(status),
        error: errorFor(status),
        updated_at: new Date().toISOString()
      });
    }, 350 + index * 700);
  });

  return reply.code(202).send({
    schema_version: SCHEMA_VERSION,
    accepted: true,
    vendor_id,
    client_request_id: parsed.data.client_request_id,
    server_time: new Date().toISOString()
  });
});

app.get("/ws", { websocket: true }, (connection) => {
  const sessionId = `session_${randomUUID()}`;
  sessions.set(sessionId, connection as WebSocketConnection);
  send(connection as WebSocketConnection, SessionReadyEventSchema.parse({
    type: "session.ready",
    schema_version: SCHEMA_VERSION,
    session_id: sessionId,
    server_time: new Date().toISOString()
  }));

  getSocket(connection as WebSocketConnection).on?.("close", () => {
    sessions.delete(sessionId);
  });
});

app.post("/__dev/scenarios/next-command-result", async (request) => {
  const body = request.body as { status?: CommandStatus };
  nextCommandResult = body.status ?? "succeeded";
  return { ok: true, next_command_result: nextCommandResult };
});

app.post("/__dev/scenarios/reset", async () => {
  devices = cloneInitialDevices();
  broadcastAllDevices();
  return { ok: true };
});

setInterval(() => {
  tick += 1;
  devices = devices.map((device) => nudgeTelemetry(device, tick));
  devices.filter((device) => device.status === "online").forEach((device) => broadcastDevice(device));
}, 2500);

const port = Number(process.env.CNS_SIMULATOR_PORT ?? 3100);
await app.listen({ host: "127.0.0.1", port });
console.log(JSON.stringify({ time: new Date().toISOString(), level: "info", message: "前端开发模拟器已启动", port }));

function broadcastAllDevices() {
  devices.forEach((device) => broadcastDevice(device));
}

function broadcastDevice(device: DeviceDetail) {
  const event = DeviceStateEventSchema.parse({
    type: "device.state",
    schema_version: SCHEMA_VERSION,
    device_id: device.device_id,
    device_type: device.device_type,
    vendor_id: device.vendor_id,
    school_name: device.school_name,
    dcdw_label: device.dcdw_label,
    status: device.status,
    event_at: new Date().toISOString(),
    last_seen_at: device.last_seen_at,
    telemetry_received_at: device.telemetry_received_at,
    latest_telemetry: device.latest_telemetry,
    change_reason: "telemetry",
    degraded: device.degraded
  });

  for (const connection of sessions.values()) {
    send(connection, event);
  }
}

function sendToSession(sessionId: string, event: BackendWebSocketEvent) {
  const connection = sessions.get(sessionId);
  if (connection) {
    send(connection, event);
  }
}

function send(connection: WebSocketConnection, event: BackendWebSocketEvent) {
  getSocket(connection).send(JSON.stringify(event));
}

function getSocket(connection: WebSocketConnection): WebSocketPeer {
  if (connection.socket) {
    return connection.socket;
  }
  return connection as WebSocketPeer;
}

function businessStatusFor(status: CommandStatus): string | null {
  if (status === "succeeded") return "accepted";
  if (status === "failed") return "rejected";
  if (status === "timeout") return "timeout";
  if (status === "delivery_uncertain") return "delivery_uncertain";
  return status;
}

function errorFor(status: CommandStatus) {
  if (status === "failed") {
    return { code: "invalid_parameter" as const, message: "模拟器拒绝了该命令" };
  }
  if (status === "timeout") {
    return { code: "service_unavailable" as const, message: "模拟器返回命令超时" };
  }
  if (status === "delivery_uncertain") {
    return { code: "mqtt_unavailable" as const, message: "模拟器返回投递结果不确定" };
  }
  return null;
}
