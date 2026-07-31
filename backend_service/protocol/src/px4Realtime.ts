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

export const Px4ProbeIdSchema = z
  .string()
  .min(8)
  .max(128)
  .regex(/^[A-Za-z0-9._:-]+$/);

export const Px4LatencyProbeRequestSchema = z
  .object({
    session_id: z.string().min(8).max(128),
    probe_id: Px4ProbeIdSchema
  })
  .strict();

export const Px4LatencyProbeMessageSchema = Px4LatencyProbeRequestSchema.extend({
  schema_version: SchemaVersionSchema,
  device_id: DeviceIdSchema
}).strict();

export const Px4LatencyAckMessageSchema = Px4LatencyProbeMessageSchema;

export const Px4LatencyAckEventSchema = Px4LatencyAckMessageSchema.extend({
  type: z.literal("px4.latency_ack")
}).strict();

export const Px4LatencyProbeAcceptedResponseSchema = z
  .object({
    schema_version: SchemaVersionSchema,
    accepted: z.literal(true),
    device_id: DeviceIdSchema,
    probe_id: Px4ProbeIdSchema
  })
  .strict();

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
export type Px4LatencyProbeRequest = z.infer<typeof Px4LatencyProbeRequestSchema>;
export type Px4LatencyProbeMessage = z.infer<typeof Px4LatencyProbeMessageSchema>;
export type Px4LatencyAckMessage = z.infer<typeof Px4LatencyAckMessageSchema>;
export type Px4LatencyAckEvent = z.infer<typeof Px4LatencyAckEventSchema>;
export type Px4LatencyProbeAcceptedResponse = z.infer<
  typeof Px4LatencyProbeAcceptedResponseSchema
>;
export type FrontendWebSocketMessage = z.infer<typeof FrontendWebSocketMessageSchema>;
