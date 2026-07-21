import type { FastifyInstance, FastifyReply } from "fastify";
import { z } from "zod";

import {
  DeviceCommandRequestSchema,
  DeviceDetailResponseSchema,
  DeviceListQuerySchema,
  DeviceListResponseSchema,
  SCHEMA_VERSION,
  VendorIdSchema,
  type CommandAcceptedResponse,
  type ErrorCode,
  type ErrorResponse
} from "@cns/backend-protocol";

import type { CommandTracker } from "../commands/commandTracker.js";
import type { WebSocketHub } from "../realtime/webSocketHub.js";
import {
  RouteServiceUnavailableError,
  type RouteServiceGateway
} from "../route/routeServiceGateway.js";
import type { DeviceStore } from "./deviceStore.js";

const DeviceParamsSchema = z.object({
  vendor_id: VendorIdSchema
});

export async function registerDeviceRoutes(
  app: FastifyInstance,
  dependencies: {
    devices: DeviceStore;
    commands: CommandTracker;
    realtime: WebSocketHub;
    routeService: RouteServiceGateway;
  }
): Promise<void> {
  const { commands, devices, realtime, routeService } = dependencies;

  app.get("/api/devices", async (request, reply) => {
    const query = DeviceListQuerySchema.safeParse(request.query);
    if (!query.success) {
      return sendError(reply, 400, "invalid_parameter", "设备查询参数不符合协议");
    }

    try {
      return DeviceListResponseSchema.parse({
        schema_version: SCHEMA_VERSION,
        items: await devices.list(query.data)
      });
    } catch {
      return sendError(reply, 503, "database_unavailable", "设备数据库暂不可用");
    }
  });

  app.get("/api/devices/:vendor_id", async (request, reply) => {
    const params = DeviceParamsSchema.safeParse(request.params);
    if (!params.success) {
      return sendError(reply, 400, "invalid_parameter", "设备编号不符合协议");
    }

    try {
      const item = await devices.get(params.data.vendor_id);
      if (!item) {
        return sendError(reply, 404, "not_found", "未找到设备");
      }

      return DeviceDetailResponseSchema.parse({
        schema_version: SCHEMA_VERSION,
        item
      });
    } catch {
      return sendError(reply, 503, "database_unavailable", "设备数据库暂不可用");
    }
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

    let item;
    try {
      item = await devices.get(params.data.vendor_id);
    } catch {
      return sendError(reply, 503, "database_unavailable", "设备数据库暂不可用");
    }
    if (!item) {
      return sendError(reply, 404, "not_found", "未找到设备");
    }
    if (item.status !== "online") {
      return sendError(reply, 409, "target_offline", "目标设备离线");
    }
    if (!realtime.hasSession(command.data.session_id)) {
      return sendError(reply, 409, "websocket_session_required", "当前 WebSocket 会话不可用，请刷新页面后重试");
    }
    if ((await routeService.dependencyStatus()) !== "ready") {
      return sendError(reply, 503, "mqtt_unavailable", "MQTT 连接暂不可用");
    }

    const acceptedAt = new Date().toISOString();
    commands.register(command.data.client_request_id, {
      sessionId: command.data.session_id,
      vendorId: params.data.vendor_id,
      commandType: command.data.type,
      command: command.data.type === "control" ? command.data.command : null
    });

    try {
      await routeService.publishCommand(params.data.vendor_id, command.data);
    } catch (error) {
      commands.forget(command.data.client_request_id);
      const message = error instanceof RouteServiceUnavailableError
        ? error.message
        : "命令发布到 route_service 失败";
      return sendError(reply, 503, "mqtt_unavailable", message);
    }

    return reply.code(202).send({
      schema_version: SCHEMA_VERSION,
      accepted: true,
      vendor_id: params.data.vendor_id,
      client_request_id: command.data.client_request_id,
      server_time: acceptedAt
    } satisfies CommandAcceptedResponse);
  });
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
