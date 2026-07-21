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
  } = {
    databaseStatus: async () => "not_configured"
  }
): Promise<void> {
  app.get("/api/health", async (): Promise<HealthResponse> => ({
    schema_version: SCHEMA_VERSION,
    status: (await dependencies.databaseStatus()) === "unavailable" ? "degraded" : "ok",
    server_time: new Date().toISOString(),
    dependencies: {
      database: await dependencies.databaseStatus(),
      mqtt: "not_configured"
    }
  }));
}
