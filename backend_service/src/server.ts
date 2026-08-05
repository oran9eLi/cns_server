import Fastify from "fastify";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";

import { createCommandTracker } from "./commands/commandTracker.js";
import { getConfigPathFromEnv } from "./config/env.js";
import { AppConfigSchema, loadAppConfig, type AppConfig } from "./config/appConfig.js";
import { createDeviceStore } from "./devices/createDeviceStore.js";
import { registerDeviceRoutes } from "./devices/deviceRoutes.js";
import { registerHealthRoutes } from "./health/healthRoutes.js";
import { createLogger } from "./logging/logger.js";
import {
  createWebSocketHub,
  type WebSocketHub
} from "./realtime/webSocketHub.js";
import {
  isTerminalRouteStatus,
  toCommandUpdatedEvent,
  toDeviceStateEvent
} from "./route/routeProtocol.js";
import { createRouteServiceGateway } from "./route/routeServiceGateway.js";

export async function buildServer(config: AppConfig = AppConfigSchema.parse({})) {
  const app = Fastify({
    logger: false
  });
  const logger = createLogger(config);
  let realtime: WebSocketHub;
  const devices = createDeviceStore(config);
  const commands = createCommandTracker(config.command.mapping_retention_ms);
  const routeService = createRouteServiceGateway(config.mqtt, {
    onDeviceState(message) {
      realtime.broadcast(toDeviceStateEvent(message));
    },
    onDeviceRealtime(message) {
      realtime.broadcastTelemetry(message.device_id, {
        type: "telemetry.realtime",
        ...message,
        server_received_at: new Date().toISOString()
      });
    },
    onCommandAck(message) {
      if (!message.request_id) return;
      const tracked = commands.get(message.request_id);
      if (!tracked) {
        logger.debug("Ignored command ACK without an active browser mapping", {
          request_id: message.request_id
        });
        return;
      }
      realtime.sendToSession(tracked.sessionId, toCommandUpdatedEvent(message, tracked));
      if (isTerminalRouteStatus(message.status)) {
        commands.complete(message.request_id);
      }
    }
  }, logger);

  realtime = createWebSocketHub((deviceId, interested) => {
    void routeService.setRealtimeInterest(deviceId, interested).catch((error) => {
      logger.warn("实时遥测订阅收敛失败", {
        device_id: deviceId,
        interested,
        error: error instanceof Error ? error.message : String(error)
      });
    });
  });

  await routeService.start();

  await realtime.register(app);
  await registerHealthRoutes(app, {
    databaseStatus: () => devices.dependencyStatus(),
    mqttStatus: () => routeService.dependencyStatus()
  });
  await registerDeviceRoutes(app, { commands, devices, realtime, routeService });

  app.addHook("onClose", async () => {
    await devices.close();
    await routeService.close();
    commands.close();
  });

  return app;
}

async function main() {
  const config = await loadAppConfig(getConfigPathFromEnv());
  const app = await buildServer(config);
  const logger = createLogger(config);

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
