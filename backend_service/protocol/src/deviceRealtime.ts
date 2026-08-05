import { z } from "zod";

import {
  DateTimeStringSchema,
  DeviceIdSchema,
  JsonObjectSchema,
  SchemaVersionSchema
} from "./common.js";

/** 设备无关的高频遥测帧，只用于实时显示，不写数据库。 */
export const DeviceRealtimeFrameSchema = z
  .object({
    schema_version: SchemaVersionSchema,
    device_id: DeviceIdSchema,
    sequence: z.number().int().nonnegative(),
    sent_at: DateTimeStringSchema,
    telemetry: JsonObjectSchema
  })
  .strict();

export const DeviceRealtimeEventSchema = DeviceRealtimeFrameSchema.extend({
  type: z.literal("telemetry.realtime"),
  server_received_at: DateTimeStringSchema
}).strict();

export const TelemetrySubscribeMessageSchema = z
  .object({
    type: z.literal("telemetry.subscribe"),
    device_id: DeviceIdSchema
  })
  .strict();

export const TelemetryUnsubscribeMessageSchema = z
  .object({
    type: z.literal("telemetry.unsubscribe"),
    device_id: DeviceIdSchema
  })
  .strict();

export const FrontendWebSocketMessageSchema = z.discriminatedUnion("type", [
  TelemetrySubscribeMessageSchema,
  TelemetryUnsubscribeMessageSchema
]);

export type DeviceRealtimeFrame = z.infer<typeof DeviceRealtimeFrameSchema>;
export type DeviceRealtimeEvent = z.infer<typeof DeviceRealtimeEventSchema>;
export type TelemetrySubscribeMessage = z.infer<
  typeof TelemetrySubscribeMessageSchema
>;
export type TelemetryUnsubscribeMessage = z.infer<
  typeof TelemetryUnsubscribeMessageSchema
>;
export type FrontendWebSocketMessage = z.infer<
  typeof FrontendWebSocketMessageSchema
>;
