import type { FastifyInstance, FastifyReply } from "fastify";
import { z } from "zod";

import {
  CommandUpdatedEventSchema,
  DeviceCommandRequestSchema,
  DeviceDetailResponseSchema,
  DeviceListQuerySchema,
  DeviceListResponseSchema,
  DeviceStateEventSchema,
  SCHEMA_VERSION,
  type CommandAcceptedResponse,
  type CommandStatus,
  type DeviceCommandRequest,
  type ErrorCode,
  type ErrorResponse
} from "@cns/backend-protocol";

import {
  businessStatusFor,
  toCommandRecord,
  type CommandStore
} from "../commands/commandStore.js";
import type { WebSocketHub } from "../realtime/webSocketHub.js";
import type { DeviceStore } from "./deviceStore.js";

const DeviceParamsSchema = z.object({
  vendor_id: z.string().min(1).max(64)
});

export async function registerDeviceRoutes(
  app: FastifyInstance,
  dependencies: {
    devices: DeviceStore;
    commands: CommandStore;
    realtime: WebSocketHub;
  }
): Promise<void> {
  const { commands, devices, realtime } = dependencies;

  app.get("/api/devices", async (request, reply) => {
    const query = DeviceListQuerySchema.safeParse(request.query);
    if (!query.success) {
      return sendError(reply, 400, "invalid_parameter", "设备查询参数不符合协议");
    }

    return DeviceListResponseSchema.parse({
      schema_version: SCHEMA_VERSION,
      items: await devices.list(query.data)
    });
  });

  app.get("/api/devices/:vendor_id", async (request, reply) => {
    const params = DeviceParamsSchema.safeParse(request.params);
    if (!params.success) {
      return sendError(reply, 400, "invalid_parameter", "设备编号不符合协议");
    }

    const item = await devices.get(params.data.vendor_id);
    if (!item) {
      return sendError(reply, 404, "not_found", "未找到设备");
    }

    return DeviceDetailResponseSchema.parse({
      schema_version: SCHEMA_VERSION,
      item
    });
  });

  app.post("/api/devices/:vendor_id/commands", async (request, reply) => {
    const params = DeviceParamsSchema.safeParse(request.params);
    if (!params.success) {
      return sendError(reply, 400, "invalid_parameter", "设备编号不符合协议");
    }

    const command = DeviceCommandRequestSchema.safeParse(request.body);
    if (!command.success) {
      return sendError(reply, 400, "invalid_parameter", "命令参数不符合协议");
    }

    const item = await devices.get(params.data.vendor_id);
    if (!item) {
      return sendError(reply, 404, "not_found", "未找到设备");
    }
    if (item.status !== "online") {
      return sendError(reply, 409, "target_offline", "目标设备离线");
    }
    if (!realtime.hasSession(command.data.session_id)) {
      return sendError(reply, 409, "websocket_session_required", "当前 WebSocket 会话不可用，请刷新页面后重试");
    }

    const acceptedAt = new Date().toISOString();
    await commands.create(toCommandRecord(params.data.vendor_id, command.data, acceptedAt));
    await emitCommandStatus(commands, realtime, params.data.vendor_id, command.data, "submitted", acceptedAt);
    await emitCommandStatus(commands, realtime, params.data.vendor_id, command.data, "dispatched");

    if (command.data.type === "control" && command.data.command === "set_motor_pwm") {
      const updated = await devices.setMotorPwm(params.data.vendor_id, command.data.parameters.motor_pwm);
      if (updated) {
        realtime.broadcast(DeviceStateEventSchema.parse({
          type: "device.state",
          schema_version: SCHEMA_VERSION,
          vendor_id: updated.vendor_id,
          status: updated.status,
          last_seen_at: updated.last_seen_at,
          telemetry_received_at: updated.telemetry_received_at,
          latest_telemetry: updated.latest_telemetry,
          degraded: updated.degraded
        }));
      }
    }

    await emitCommandStatus(commands, realtime, params.data.vendor_id, command.data, "succeeded");

    return reply.code(202).send({
      schema_version: SCHEMA_VERSION,
      accepted: true,
      vendor_id: params.data.vendor_id,
      client_request_id: command.data.client_request_id,
      server_time: acceptedAt
    } satisfies CommandAcceptedResponse);
  });
}

async function emitCommandStatus(
  commands: CommandStore,
  realtime: WebSocketHub,
  vendorId: string,
  command: DeviceCommandRequest,
  status: CommandStatus,
  updatedAt = new Date().toISOString()
): Promise<void> {
  const businessStatus = businessStatusFor(status);
  await commands.updateStatus(command.session_id, command.client_request_id, status, businessStatus, null);
  realtime.sendToSession(command.session_id, CommandUpdatedEventSchema.parse({
    type: "command.updated",
    schema_version: SCHEMA_VERSION,
    client_request_id: command.client_request_id,
    vendor_id: vendorId,
    command_type: command.type,
    command: command.type === "control" ? command.command : null,
    status,
    business_status: businessStatus,
    error: null,
    updated_at: updatedAt
  }));
}

function sendError(
  reply: FastifyReply,
  statusCode: number,
  code: ErrorCode,
  message: string
): FastifyReply {
  const response: ErrorResponse = {
    error: {
      code,
      message
    }
  };
  return reply.code(statusCode).send(response);
}
