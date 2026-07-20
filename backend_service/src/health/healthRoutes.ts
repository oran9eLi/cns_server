import type { FastifyInstance } from "fastify";

import {
  SCHEMA_VERSION,
  type HealthResponse
} from "@cns/backend-protocol";

export async function registerHealthRoutes(app: FastifyInstance): Promise<void> {
  app.get("/api/health", async (): Promise<HealthResponse> => ({
    schema_version: SCHEMA_VERSION,
    status: "ok",
    server_time: new Date().toISOString(),
    dependencies: {
      database: "not_configured",
      mqtt: "not_configured"
    }
  }));
}
