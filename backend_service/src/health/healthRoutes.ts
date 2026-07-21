import type { FastifyInstance } from "fastify";

import {
  SCHEMA_VERSION,
  type DependencyStatus,
  type HealthResponse
} from "@cns/backend-protocol";

export async function registerHealthRoutes(
  app: FastifyInstance,
  dependencies: {
    databaseStatus: () => Promise<DependencyStatus>;
    mqttStatus: () => Promise<DependencyStatus>;
  } = {
    databaseStatus: async () => "not_configured",
    mqttStatus: async () => "not_configured"
  }
): Promise<void> {
  app.get("/api/health", async (): Promise<HealthResponse> => {
    const [database, mqtt] = await Promise.all([
      dependencies.databaseStatus(),
      dependencies.mqttStatus()
    ]);
    return {
      schema_version: SCHEMA_VERSION,
      status: database === "unavailable" || mqtt === "unavailable" ? "degraded" : "ok",
      server_time: new Date().toISOString(),
      dependencies: {
        database,
        mqtt
      }
    };
  });
}
