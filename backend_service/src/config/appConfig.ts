import { readFile } from "node:fs/promises";

import { z } from "zod";

export const AppConfigSchema = z
  .object({
    env: z.enum(["development", "test", "production"]).default("development"),
    http: z
      .object({
        host: z.string().min(1).default("127.0.0.1"),
        port: z.number().int().min(1).max(65535).default(3000)
      })
      .strict()
      .default({}),
    logging: z
      .object({
        level: z.enum(["debug", "info", "warn", "error"]).default("info")
      })
      .strict()
      .default({}),
    database: z
      .object({
        connection_string: z.string().url().optional(),
        max_connections: z.number().int().min(1).max(50).default(10),
        ssl: z.boolean().default(false)
      })
      .strict()
      .default({}),
    mqtt: z
      .object({
        protocol: z.enum(["mqtt", "mqtts"]).default("mqtt"),
        host: z.string().min(1).default("127.0.0.1"),
        port: z.number().int().min(1).max(65535).default(1883),
        client_id: z.string().min(1).max(128).default("cns-backend-service"),
        username: z.string().optional(),
        password: z.string().optional(),
        topic_namespace: z.string().min(1).max(128).default("cns_rpi"),
        source_id: z.string().regex(/^[A-Za-z0-9._-]+$/).default("web-console"),
        connect_timeout_ms: z.number().int().min(1000).max(60000).default(10000),
        publish_timeout_ms: z.number().int().min(500).max(30000).default(5000),
        reconnect_period_ms: z.number().int().min(100).max(30000).default(1000)
      })
      .strict()
      .optional(),
    command: z
      .object({
        mapping_retention_ms: z.number().int().min(1000).max(300000).default(60000)
      })
      .strict()
      .default({})
  })
  .strict();

export type AppConfig = z.infer<typeof AppConfigSchema>;

export async function loadAppConfig(configPath?: string): Promise<AppConfig> {
  if (!configPath) {
    return AppConfigSchema.parse({});
  }

  const rawConfig = await readFile(configPath, "utf8");
  return AppConfigSchema.parse(JSON.parse(rawConfig));
}
