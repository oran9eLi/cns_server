import { z } from "zod";

import { DateTimeStringSchema, SchemaVersionSchema } from "./common.js";
import {
  CommandAcceptedResponseSchema,
  DeviceCommandRequestSchema
} from "./commands.js";
import {
  DeviceDetailResponseSchema,
  DeviceListQuerySchema,
  DeviceListResponseSchema
} from "./devices.js";
import {
  Px4LatencyProbeAcceptedResponseSchema,
  Px4LatencyProbeRequestSchema
} from "./px4Realtime.js";

export const HealthStatusSchema = z.enum(["ok", "degraded"]);

export const DependencyStatusSchema = z.enum([
  "ready",
  "degraded",
  "unavailable",
  "not_configured"
]);

export const HealthResponseSchema = z
  .object({
    schema_version: SchemaVersionSchema,
    status: HealthStatusSchema,
    server_time: DateTimeStringSchema,
    dependencies: z
      .object({
        database: DependencyStatusSchema,
        mqtt: DependencyStatusSchema
      })
      .strict()
  })
  .strict();

export const REST_ENDPOINTS = {
  health: "/api/health",
  devices: "/api/devices",
  deviceDetail: "/api/devices/:vendor_id",
  deviceCommands: "/api/devices/:vendor_id/commands",
  px4LatencyProbes: "/api/devices/:vendor_id/px4-latency-probes"
} as const;

export const RestSchemas = {
  health: {
    response: HealthResponseSchema
  },
  listDevices: {
    query: DeviceListQuerySchema,
    response: DeviceListResponseSchema
  },
  getDevice: {
    response: DeviceDetailResponseSchema
  },
  postDeviceCommand: {
    body: DeviceCommandRequestSchema,
    response: CommandAcceptedResponseSchema
  },
  postPx4LatencyProbe: {
    body: Px4LatencyProbeRequestSchema,
    response: Px4LatencyProbeAcceptedResponseSchema
  }
} as const;

export type HealthStatus = z.infer<typeof HealthStatusSchema>;
export type DependencyStatus = z.infer<typeof DependencyStatusSchema>;
export type HealthResponse = z.infer<typeof HealthResponseSchema>;
export type RestEndpoint = (typeof REST_ENDPOINTS)[keyof typeof REST_ENDPOINTS];
