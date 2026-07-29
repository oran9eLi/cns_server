import { z } from "zod";

export const SCHEMA_VERSION = 1 as const;

export const SchemaVersionSchema = z.literal(SCHEMA_VERSION);

export const DateTimeStringSchema = z.string().datetime({ offset: true });

export const DeviceIdSchema = z
  .string()
  .min(1)
  .max(64)
  .regex(/^[A-Za-z0-9._:-]+$/);

/** 兼容旧 API 名称；现在同时接受主控箱编号和 PX4 设备 ID。 */
export const VendorIdSchema = DeviceIdSchema;

export type JsonValue =
  | null
  | boolean
  | number
  | string
  | JsonValue[]
  | { [key: string]: JsonValue };

export const JsonValueSchema: z.ZodType<JsonValue> = z.lazy(() =>
  z.union([
    z.null(),
    z.boolean(),
    z.number().finite(),
    z.string(),
    z.array(JsonValueSchema),
    z.record(JsonValueSchema)
  ])
);

export const JsonObjectSchema = z.record(JsonValueSchema);

export const ErrorCodeSchema = z
  .string()
  .min(1)
  .max(64)
  .regex(/^[A-Za-z0-9._-]+$/);

export const ErrorPayloadSchema = z
  .object({
    code: ErrorCodeSchema,
    message: z.string().min(1),
    details: JsonObjectSchema.optional()
  })
  .strict();

export const ErrorResponseSchema = z
  .object({
    error: ErrorPayloadSchema
  })
  .strict();

export type SchemaVersion = z.infer<typeof SchemaVersionSchema>;
export type DateTimeString = z.infer<typeof DateTimeStringSchema>;
export type ErrorCode = z.infer<typeof ErrorCodeSchema>;
export type ErrorPayload = z.infer<typeof ErrorPayloadSchema>;
export type ErrorResponse = z.infer<typeof ErrorResponseSchema>;
