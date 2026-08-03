import { z } from "zod";

import {
  DateTimeStringSchema,
  ErrorPayloadSchema,
  SchemaVersionSchema,
  DeviceIdSchema
} from "./common.js";

export const SessionIdSchema = z.string().min(8).max(128);

export const ClientRequestIdSchema = z.string().uuid();

export const CommandTypeSchema = z.enum(["config", "control"]);

export const ControlCommandNameSchema = z.enum([
  "set_motor_pwm",
  "takeoff",
  "land",
  "emergency_stop"
]);

export const CommandStatusSchema = z.enum([
  "submitted",
  "dispatched",
  "in_progress",
  "succeeded",
  "failed",
  "timeout",
  "delivery_uncertain"
]);

export const RuntimeConfigParametersSchema = z
  .object({
    telemetry_publish_interval_ms: z.number().int().min(100).max(60000).optional(),
    heartbeat_interval_ms: z.number().int().min(100).max(60000).optional(),
    mqtt_reconnect_delay_s: z.number().int().min(1).max(3600).optional(),
    mqtt_reconnect_delay_max_s: z.number().int().min(1).max(3600).optional()
  })
  .strict()
  .refine((value) => Object.keys(value).length > 0, {
    message: "At least one runtime config parameter is required."
  })
  .refine(
    (value) =>
      value.mqtt_reconnect_delay_s === undefined ||
      value.mqtt_reconnect_delay_max_s === undefined ||
      value.mqtt_reconnect_delay_s <= value.mqtt_reconnect_delay_max_s,
    {
      message: "MQTT reconnect delay must not exceed its maximum."
    }
  );

export const PwmValueSchema = z.number().int().min(1000).max(2000);

export const SetMotorPwmParametersSchema = z
  .object({
    pwm_us: z.tuple([
      PwmValueSchema,
      PwmValueSchema,
      PwmValueSchema,
      PwmValueSchema
    ])
  })
  .strict();

export const EmptyControlParametersSchema = z.object({}).strict();

const BaseCommandRequestSchema = z
  .object({
    session_id: SessionIdSchema,
    client_request_id: ClientRequestIdSchema
  })
  .strict();

export const ConfigCommandRequestSchema = BaseCommandRequestSchema.extend({
  type: z.literal("config"),
  parameters: RuntimeConfigParametersSchema
}).strict();

export const SetMotorPwmCommandRequestSchema = BaseCommandRequestSchema.extend({
  type: z.literal("control"),
  command: z.literal("set_motor_pwm"),
  parameters: SetMotorPwmParametersSchema
}).strict();

export const TakeoffCommandRequestSchema = BaseCommandRequestSchema.extend({
  type: z.literal("control"),
  command: z.literal("takeoff"),
  parameters: EmptyControlParametersSchema
}).strict();

export const LandCommandRequestSchema = BaseCommandRequestSchema.extend({
  type: z.literal("control"),
  command: z.literal("land"),
  parameters: EmptyControlParametersSchema
}).strict();

export const EmergencyStopCommandRequestSchema = BaseCommandRequestSchema.extend({
  type: z.literal("control"),
  command: z.literal("emergency_stop"),
  parameters: EmptyControlParametersSchema
}).strict();

export const DeviceCommandRequestSchema = z.union([
  ConfigCommandRequestSchema,
  SetMotorPwmCommandRequestSchema,
  TakeoffCommandRequestSchema,
  LandCommandRequestSchema,
  EmergencyStopCommandRequestSchema
]);

export const CommandAcceptedResponseSchema = z
  .object({
    schema_version: SchemaVersionSchema,
    accepted: z.literal(true),
    device_id: DeviceIdSchema,
    client_request_id: ClientRequestIdSchema,
    server_time: DateTimeStringSchema
  })
  .strict();

export const CommandUpdatedEventSchema = z
  .object({
    type: z.literal("command.updated"),
    schema_version: SchemaVersionSchema,
    client_request_id: ClientRequestIdSchema,
    command_id: z.string().uuid().nullable(),
    device_id: DeviceIdSchema,
    command_type: CommandTypeSchema,
    command: ControlCommandNameSchema.nullable(),
    status: CommandStatusSchema,
    business_status: z.string().min(1).max(64).nullable(),
    error: ErrorPayloadSchema.nullable(),
    updated_at: DateTimeStringSchema
  })
  .strict();

export type SessionId = z.infer<typeof SessionIdSchema>;
export type ClientRequestId = z.infer<typeof ClientRequestIdSchema>;
export type CommandType = z.infer<typeof CommandTypeSchema>;
export type ControlCommandName = z.infer<typeof ControlCommandNameSchema>;
export type CommandStatus = z.infer<typeof CommandStatusSchema>;
export type RuntimeConfigParameters = z.infer<typeof RuntimeConfigParametersSchema>;
export type SetMotorPwmParameters = z.infer<typeof SetMotorPwmParametersSchema>;
export type DeviceCommandRequest = z.infer<typeof DeviceCommandRequestSchema>;
export type ConfigCommandRequest = z.infer<typeof ConfigCommandRequestSchema>;
export type SetMotorPwmCommandRequest = z.infer<typeof SetMotorPwmCommandRequestSchema>;
export type TakeoffCommandRequest = z.infer<typeof TakeoffCommandRequestSchema>;
export type LandCommandRequest = z.infer<typeof LandCommandRequestSchema>;
export type EmergencyStopCommandRequest = z.infer<typeof EmergencyStopCommandRequestSchema>;
export type CommandAcceptedResponse = z.infer<typeof CommandAcceptedResponseSchema>;
export type CommandUpdatedEvent = z.infer<typeof CommandUpdatedEventSchema>;
