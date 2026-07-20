import { z } from "zod";

export const SCHEMA_VERSION = 1 as const;

export const SchemaVersionSchema = z.literal(SCHEMA_VERSION);

export const DateTimeStringSchema = z.string().datetime({ offset: true });

export const VendorIdSchema = z.string().min(1).max(64);

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

export const ErrorCodeSchema = z.enum([
  "bad_request",
  "invalid_parameter",
  "not_found",
  "websocket_session_required",
  "idempotency_conflict",
  "target_offline",
  "database_unavailable",
  "mqtt_unavailable",
  "service_unavailable",
  "internal_error"
]);

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
