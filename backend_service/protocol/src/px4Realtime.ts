import { z } from "zod";

import {
  DateTimeStringSchema,
  DeviceIdSchema,
  JsonObjectSchema,
  SchemaVersionSchema
} from "./common.js";

/**
 * 树莓派发布的 PX4 高频快照。该消息只用于实时显示，不写数据库。
 */
export const Px4RealtimeFrameSchema = z
  .object({
    schema_version: SchemaVersionSchema,
    device_id: DeviceIdSchema,
    sequence: z.number().int().nonnegative(),
    sent_at: DateTimeStringSchema,
    telemetry: JsonObjectSchema
  })
  .strict();

export const Px4RealtimeEventSchema = Px4RealtimeFrameSchema.extend({
  type: z.literal("px4.realtime"),
  server_received_at: DateTimeStringSchema
}).strict();

export const Px4SubscribeMessageSchema = z
  .object({
    type: z.literal("px4.subscribe"),
    device_id: DeviceIdSchema
  })
  .strict();

export const Px4UnsubscribeMessageSchema = z
  .object({
    type: z.literal("px4.unsubscribe"),
    device_id: DeviceIdSchema
  })
  .strict();

export const FrontendWebSocketMessageSchema = z.discriminatedUnion("type", [
  Px4SubscribeMessageSchema,
  Px4UnsubscribeMessageSchema
]);

export type Px4RealtimeFrame = z.infer<typeof Px4RealtimeFrameSchema>;
export type Px4RealtimeEvent = z.infer<typeof Px4RealtimeEventSchema>;
export type FrontendWebSocketMessage = z.infer<typeof FrontendWebSocketMessageSchema>;
