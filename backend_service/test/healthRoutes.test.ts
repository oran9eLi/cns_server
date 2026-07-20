import { describe, expect, it } from "vitest";

import { HealthResponseSchema } from "@cns/backend-protocol";

import { buildServer } from "../src/server.js";

describe("健康检查", () => {
  it("返回符合协议的基础状态", async () => {
    const app = await buildServer();

    const response = await app.inject({
      method: "GET",
      url: "/api/health"
    });

    await app.close();

    expect(response.statusCode).toBe(200);
    expect(() => HealthResponseSchema.parse(response.json())).not.toThrow();
    expect(response.json()).toMatchObject({
      schema_version: 1,
      status: "ok",
      dependencies: {
        database: "not_configured",
        mqtt: "not_configured"
      }
    });
  });
});
