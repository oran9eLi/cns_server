import { z } from "zod";

import {
  CommandUpdatedEventSchema,
  DeviceIdSchema,
  DeviceStateEventSchema,
  DeviceTypeSchema,
  SCHEMA_VERSION,
  type CommandStatus,
  type CommandUpdatedEvent,
  type DeviceCommandRequest,
  type DeviceStateEvent,
  type ErrorPayload
} from "@cns/backend-protocol";

import type { TrackedCommand } from "../commands/commandTracker.js";
import { enrichTelemetryCoordinates } from "../telemetry/coordinateTransform.js";

const NullableDateTimeSchema = z.string().datetime({ offset: true }).nullable();

const RouteDeviceDirectoryEntrySchema = z
  .object({
    device_id: DeviceIdSchema,
    device_type: DeviceTypeSchema,
    school_name: z.string().min(1).nullable(),
    dcdw_label: z.string().min(1).nullable(),
    model_version: z.string().min(1),
    capabilities: z.array(z.string().min(1)).nullable(),
    product: z.record(z.unknown()).nullable(),
    version: z.record(z.unknown()).nullable(),
    status: z.enum(["online", "offline"])
  })
  .strict();

export const RouteDeviceDirectorySnapshotSchema = z
  .object({
    schema_version: z.literal(SCHEMA_VERSION),
    event_type: z.literal("device_directory_snapshot"),
    generated_at: z.string().datetime({ offset: true }),
    revision: z.number().int().nonnegative(),
    devices: z.array(RouteDeviceDirectoryEntrySchema)
  })
  .strict();

export const RouteDeviceStateMessageSchema = z
  .object({
    schema_version: z.literal(SCHEMA_VERSION),
    event_type: z.literal("device_state"),
    event_at: z.string().datetime({ offset: true }),
    revision: z.number().int().nonnegative(),
    device_id: DeviceIdSchema,
    device_type: DeviceTypeSchema,
    school_name: z.string().min(1).nullable(),
    dcdw_label: z.string().min(1).nullable(),
    model_version: z.string().min(1),
    capabilities: z.array(z.string().min(1)).nullable(),
    product: z.record(z.unknown()).nullable(),
    version: z.record(z.unknown()).nullable(),
    status: z.enum(["online", "offline"]),
    last_seen_at: NullableDateTimeSchema,
    telemetry_received_at: NullableDateTimeSchema,
    latest_telemetry: z.record(z.unknown()).nullable(),
    change_reason: z.enum([
      "registration_online",
      "registration_offline",
      "telemetry",
      "activity_timeout",
      "database_recovered",
      "snapshot_replay"
    ]),
    degraded: z.boolean()
  })
  .strict();

export const RouteCommandAckSchema = z
  .object({
    schema_version: z.literal(SCHEMA_VERSION),
    request_id: z.string().min(1).max(128).nullable(),
    command_id: z.string().uuid().nullable(),
    command_type: z.enum(["config", "control"]),
    status: z.enum([
      "pending",
      "dispatched",
      "in_progress",
      "succeeded",
      "failed",
      "timeout",
      "delivery_uncertain"
    ]),
    business_status: z.string().min(1).max(64).nullable(),
    occurred_at: z.string().datetime({ offset: true }),
    error: z
      .object({
        code: z.string().min(1).max(64),
        message: z.string().min(1).nullable()
      })
      .nullable()
      .optional()
  })
  .passthrough();

export type RouteDeviceStateMessage = z.infer<typeof RouteDeviceStateMessageSchema>;
export type RouteDeviceDirectorySnapshot = z.infer<
  typeof RouteDeviceDirectorySnapshotSchema
>;
export type RouteCommandAck = z.infer<typeof RouteCommandAckSchema>;

export function buildRouteCommandRequest(deviceId: string, request: DeviceCommandRequest) {
  const base = {
    schema_version: SCHEMA_VERSION,
    request_id: request.client_request_id,
    target: { device_id: deviceId },
    parameters: request.parameters
  };

  return request.type === "config"
    ? base
    : {
        ...base,
        command: request.command
      };
}

export function toDeviceStateEvent(message: RouteDeviceStateMessage): DeviceStateEvent {
  return DeviceStateEventSchema.parse({
    type: "device.state",
    schema_version: message.schema_version,
    device_id: message.device_id,
    device_type: message.device_type,
    school_name: message.school_name,
    dcdw_label: message.dcdw_label,
    capabilities: message.capabilities,
    product: message.product,
    version: message.version,
    status: message.status,
    event_at: message.event_at,
    last_seen_at: message.last_seen_at,
    telemetry_received_at: message.telemetry_received_at,
    latest_telemetry: enrichTelemetryCoordinates(message.latest_telemetry),
    change_reason: message.change_reason,
    degraded: message.degraded
  });
}

export function toCommandUpdatedEvent(
  ack: RouteCommandAck,
  tracked: TrackedCommand
): CommandUpdatedEvent {
  return CommandUpdatedEventSchema.parse({
    type: "command.updated",
    schema_version: ack.schema_version,
    client_request_id: ack.request_id,
    command_id: ack.command_id,
    device_id: tracked.deviceId,
    command_type: tracked.commandType,
    command: tracked.command,
    status: mapRouteCommandStatus(ack.status),
    business_status: ack.business_status,
    error: mapRouteError(ack.error),
    updated_at: ack.occurred_at
  });
}

export function mapRouteCommandStatus(status: RouteCommandAck["status"]): CommandStatus {
  return status === "pending" ? "submitted" : status;
}

export function isTerminalRouteStatus(status: RouteCommandAck["status"]): boolean {
  return ["succeeded", "failed", "timeout", "delivery_uncertain"].includes(status);
}

function mapRouteError(error: RouteCommandAck["error"]): ErrorPayload | null {
  if (!error) return null;
  return {
    code: error.code,
    message: error.message ?? "route_service rejected the command"
  };
}
