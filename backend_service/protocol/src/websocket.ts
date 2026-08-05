import { z } from "zod";

import {
  DateTimeStringSchema,
  SchemaVersionSchema
} from "./common.js";
import { CommandUpdatedEventSchema } from "./commands.js";
import { DeviceRealtimeEventSchema } from "./deviceRealtime.js";
import { DeviceStateEventSchema } from "./devices.js";

export const WEBSOCKET_PATH = "/ws" as const;

export const SessionReadyEventSchema = z
  .object({
    type: z.literal("session.ready"),
    schema_version: SchemaVersionSchema,
    session_id: z.string().min(8).max(128),
    server_time: DateTimeStringSchema
  })
  .strict();

export const BackendWebSocketEventSchema = z.discriminatedUnion("type", [
  SessionReadyEventSchema,
  DeviceStateEventSchema,
  CommandUpdatedEventSchema,
  DeviceRealtimeEventSchema
]);

export type SessionReadyEvent = z.infer<typeof SessionReadyEventSchema>;
export type BackendWebSocketEvent = z.infer<typeof BackendWebSocketEventSchema>;
