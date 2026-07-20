import type { AppConfig } from "../config/appConfig.js";
import { toPoolConfig } from "../commands/createCommandStore.js";
import { createInMemoryDeviceStore, type DeviceStore } from "./deviceStore.js";
import { createPostgresDeviceStore } from "./postgresDeviceStore.js";
import { createSeedDevices } from "./seedDevices.js";

export function createDeviceStore(config: AppConfig): DeviceStore {
  if (!config.database.connection_string) {
    return createInMemoryDeviceStore(createSeedDevices());
  }

  return createPostgresDeviceStore(toPoolConfig(config));
}
