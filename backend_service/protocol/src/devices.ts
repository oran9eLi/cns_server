import { z } from "zod";

import {
  DateTimeStringSchema,
  JsonObjectSchema,
  SchemaVersionSchema,
  VendorIdSchema
} from "./common.js";

export const DeviceStatusSchema = z.enum(["online", "offline"]);

export const DeviceStateChangeReasonSchema = z.enum([
  "registration_online",
  "registration_offline",
  "telemetry",
  "activity_timeout",
  "database_recovered",
  "snapshot_replay"
]);

export const DeviceListQuerySchema = z
  .object({
    keyword: z.string().trim().min(1).max(128).optional(),
    school_name: z.string().trim().min(1).max(128).optional(),
    status: DeviceStatusSchema.optional()
  })
  .strict();

export const DeviceSummarySchema = z
  .object({
    vendor_id: VendorIdSchema,
    school_name: z.string().min(1),
    dcdw_label: z.string().min(1).nullable(),
    model_version: z.string().min(1),
    status: DeviceStatusSchema,
    last_seen_at: DateTimeStringSchema.nullable(),
    telemetry_received_at: DateTimeStringSchema.nullable(),
    degraded: z.boolean()
  })
  .strict();

export const DeviceDetailSchema = DeviceSummarySchema.extend({
  provisioned_at: DateTimeStringSchema.nullable(),
  latest_telemetry: JsonObjectSchema.nullable()
}).strict();

export const DeviceListResponseSchema = z
  .object({
    schema_version: SchemaVersionSchema,
    items: z.array(DeviceSummarySchema)
  })
  .strict();

export const DeviceDetailResponseSchema = z
  .object({
    schema_version: SchemaVersionSchema,
    item: DeviceDetailSchema
  })
  .strict();

export const DeviceStateEventSchema = z
  .object({
    type: z.literal("device.state"),
    schema_version: SchemaVersionSchema,
    vendor_id: VendorIdSchema,
    school_name: z.string().min(1),
    dcdw_label: z.string().min(1).nullable(),
    status: DeviceStatusSchema,
    event_at: DateTimeStringSchema,
    last_seen_at: DateTimeStringSchema.nullable(),
    telemetry_received_at: DateTimeStringSchema.nullable(),
    latest_telemetry: JsonObjectSchema.nullable(),
    change_reason: DeviceStateChangeReasonSchema,
    degraded: z.boolean()
  })
  .strict();

export type DeviceStatus = z.infer<typeof DeviceStatusSchema>;
export type DeviceStateChangeReason = z.infer<typeof DeviceStateChangeReasonSchema>;
export type DeviceListQuery = z.infer<typeof DeviceListQuerySchema>;
export type DeviceSummary = z.infer<typeof DeviceSummarySchema>;
export type DeviceDetail = z.infer<typeof DeviceDetailSchema>;
export type DeviceListResponse = z.infer<typeof DeviceListResponseSchema>;
export type DeviceDetailResponse = z.infer<typeof DeviceDetailResponseSchema>;
export type DeviceStateEvent = z.infer<typeof DeviceStateEventSchema>;
