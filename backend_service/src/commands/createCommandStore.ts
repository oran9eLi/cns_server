import type { PoolConfig } from "pg";

import type { AppConfig } from "../config/appConfig.js";
import { createInMemoryCommandStore, type CommandStore } from "./commandStore.js";
import { createPostgresCommandStore } from "./postgresCommandStore.js";

export function createCommandStore(config: AppConfig): CommandStore {
  if (!config.database.connection_string) {
    return createInMemoryCommandStore();
  }

  return createPostgresCommandStore(toPoolConfig(config));
}

export function toPoolConfig(config: AppConfig): PoolConfig {
  return {
    connectionString: config.database.connection_string,
    max: config.database.max_connections,
    ssl: config.database.ssl ? { rejectUnauthorized: true } : undefined
  };
}
