import Fastify from "fastify";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";

import { getConfigPathFromEnv } from "./config/env.js";
import { loadAppConfig } from "./config/appConfig.js";
import { registerHealthRoutes } from "./health/healthRoutes.js";
import { createLogger } from "./logging/logger.js";

export async function buildServer() {
  const app = Fastify({
    logger: false
  });

  await registerHealthRoutes(app);

  return app;
}

async function main() {
  const config = await loadAppConfig(getConfigPathFromEnv());
  const logger = createLogger(config);
  const app = await buildServer();

  const close = async (signal: NodeJS.Signals) => {
    logger.info("收到退出信号，正在关闭后端服务", { signal });
    await app.close();
    process.exit(0);
  };

  process.once("SIGINT", close);
  process.once("SIGTERM", close);

  await app.listen({
    host: config.http.host,
    port: config.http.port
  });

  logger.info("后端服务已启动", {
    host: config.http.host,
    port: config.http.port
  });
}

if (process.argv[1] && fileURLToPath(import.meta.url) === resolve(process.argv[1])) {
  main().catch((error: unknown) => {
    console.error(
      JSON.stringify({
        time: new Date().toISOString(),
        level: "error",
        message: "后端服务启动失败",
        error: error instanceof Error ? error.message : String(error)
      })
    );
    process.exit(1);
  });
}
