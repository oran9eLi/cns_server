import type { PoolConfig } from "pg";

import type { AppConfig } from "../config/appConfig.js";
import { createInMemoryDeviceStore, type DeviceStore } from "./deviceStore.js";
import { createPostgresDeviceStore } from "./postgresDeviceStore.js";
import { createSeedDevices } from "./seedDevices.js";

export function createDeviceStore(config: AppConfig): DeviceStore {
  if (!config.database.connection_string) {
    return createInMemoryDeviceStore(createSeedDevices());
  }

  return createPostgresDeviceStore(toPoolConfig(config));
}

function toPoolConfig(config: AppConfig): PoolConfig {
  return {
    connectionString: config.database.connection_string,
    max: config.database.max_connections,
    ssl: config.database.ssl ? { rejectUnauthorized: true } : undefined
  };
}
