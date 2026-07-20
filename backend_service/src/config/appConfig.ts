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
