import { connect, type IClientOptions, type MqttClient } from "mqtt";

import type {
  DependencyStatus,
  DeviceCommandRequest,
  Px4LatencyAckMessage,
  Px4RealtimeFrame
} from "@cns/backend-protocol";
import {
  Px4LatencyAckMessageSchema,
  Px4RealtimeFrameSchema,
  SCHEMA_VERSION
} from "@cns/backend-protocol";

import type { AppConfig } from "../config/appConfig.js";
import type { Logger } from "../logging/logger.js";
import {
  RouteCommandAckSchema,
  RouteDeviceStateMessageSchema,
  buildRouteCommandRequest,
  type RouteCommandAck,
  type RouteDeviceStateMessage
} from "./routeProtocol.js";

type MqttConfig = NonNullable<AppConfig["mqtt"]>;

export interface RouteServiceGateway {
  start(): Promise<void>;
  publishCommand(vendorId: string, request: DeviceCommandRequest): Promise<void>;
  publishPx4LatencyProbe(
    deviceId: string,
    sessionId: string,
    probeId: string
  ): Promise<void>;
  dependencyStatus(): Promise<DependencyStatus>;
  close(): Promise<void>;
}

export type RouteServiceHandlers = {
  onDeviceState(message: RouteDeviceStateMessage): void;
  onCommandAck(message: RouteCommandAck): void;
  onPx4Realtime(message: Px4RealtimeFrame): void;
  onPx4LatencyAck(message: Px4LatencyAckMessage): void;
};

export class RouteServiceUnavailableError extends Error {}

export function createRouteServiceGateway(
  config: AppConfig["mqtt"],
  handlers: RouteServiceHandlers,
  logger: Logger
): RouteServiceGateway {
  if (!config) return createUnconfiguredGateway();
  return createMqttGateway(config, handlers, logger);
}

function createUnconfiguredGateway(): RouteServiceGateway {
  return {
    async start() {
      return undefined;
    },
    async publishCommand() {
      throw new RouteServiceUnavailableError("MQTT is not configured");
    },
    async publishPx4LatencyProbe() {
      throw new RouteServiceUnavailableError("MQTT is not configured");
    },
    async dependencyStatus() {
      return "not_configured";
    },
    async close() {
      return undefined;
    }
  };
}

function createMqttGateway(
  config: MqttConfig,
  handlers: RouteServiceHandlers,
  logger: Logger
): RouteServiceGateway {
  let client: MqttClient | null = null;
  let status: DependencyStatus = "unavailable";
  const topics = buildTopics(config);

  return {
    async start() {
      if (client) return;
      client = connect(`${config.protocol}://${config.host}:${config.port}`, clientOptions(config));
      client.on("message", (topic, payload) => {
        handleMessage(topic, payload.toString("utf8"), topics, handlers, logger);
      });
      client.on("close", () => {
        status = "unavailable";
      });
      client.on("offline", () => {
        status = "unavailable";
      });
      client.on("error", (error) => {
        logger.warn("MQTT connection error", { error: error.message });
      });

      try {
        await waitUntilSubscribed(client, config, topics, () => {
          status = "ready";
        });
      } catch (error) {
        const failedClient = client;
        client = null;
        status = "unavailable";
        await new Promise<void>((resolve) => failedClient.end(true, {}, () => resolve()));
        throw error;
      }
    },
    async publishCommand(vendorId, request) {
      if (!client || status !== "ready") {
        throw new RouteServiceUnavailableError("MQTT is unavailable");
      }

      const topic = request.type === "config" ? topics.configRequest : topics.controlRequest;
      const payload = JSON.stringify(buildRouteCommandRequest(vendorId, request));
      await publishWithTimeout(client, topic, payload, config.publish_timeout_ms);
    },
    async publishPx4LatencyProbe(deviceId, sessionId, probeId) {
      if (!client || status !== "ready") {
        throw new RouteServiceUnavailableError("MQTT is unavailable");
      }
      await publishQos0WithTimeout(
        client,
        `${config.topic_namespace}/${deviceId}/px4/latency/probe/v1`,
        JSON.stringify({
          schema_version: SCHEMA_VERSION,
          device_id: deviceId,
          session_id: sessionId,
          probe_id: probeId
        }),
        config.publish_timeout_ms
      );
    },
    async dependencyStatus() {
      return status;
    },
    async close() {
      const activeClient = client;
      client = null;
      status = "unavailable";
      if (!activeClient) return;
      await new Promise<void>((resolve, reject) => {
        activeClient.end(false, {}, (error) => {
          if (error) reject(error);
          else resolve();
        });
      });
    }
  };
}

function clientOptions(config: MqttConfig): IClientOptions {
  return {
    clientId: config.client_id,
    username: config.username || undefined,
    password: config.password || undefined,
    clean: true,
    connectTimeout: config.connect_timeout_ms,
    reconnectPeriod: config.reconnect_period_ms,
    resubscribe: false
  };
}

function buildTopics(config: MqttConfig) {
  const sourcePrefix = `${config.topic_namespace}/sources/${config.source_id}`;
  return {
    stateFilter: `${config.topic_namespace}/events/devices/+/state`,
    statePrefix: `${config.topic_namespace}/events/devices/`,
    px4RealtimeFilter: `${config.topic_namespace}/+/px4/realtime/v1`,
    px4RealtimePrefix: `${config.topic_namespace}/`,
    px4LatencyAckFilter: `${config.topic_namespace}/+/px4/latency/ack/v1`,
    configRequest: `${sourcePrefix}/config/request`,
    configAck: `${sourcePrefix}/config/ack`,
    controlRequest: `${sourcePrefix}/control/request`,
    controlAck: `${sourcePrefix}/control/ack`
  };
}

async function waitUntilSubscribed(
  client: MqttClient,
  config: MqttConfig,
  topics: ReturnType<typeof buildTopics>,
  onReady: () => void
): Promise<void> {
  await new Promise<void>((resolve, reject) => {
    let settled = false;
    const timeout = setTimeout(() => {
      if (settled) return;
      settled = true;
      reject(new RouteServiceUnavailableError("MQTT connection timed out"));
    }, config.connect_timeout_ms);

    client.on("connect", () => {
      client.subscribe(
        {
          [topics.stateFilter]: { qos: 0 },
          [topics.px4RealtimeFilter]: { qos: 0 },
          [topics.px4LatencyAckFilter]: { qos: 0 },
          [topics.configAck]: { qos: 2 },
          [topics.controlAck]: { qos: 2 }
        },
        (error) => {
          if (error) {
            if (!settled) {
              settled = true;
              clearTimeout(timeout);
              reject(error);
            }
            return;
          }
          onReady();
          if (!settled) {
            settled = true;
            clearTimeout(timeout);
            resolve();
          }
        }
      );
    });
  });
}

async function publishWithTimeout(
  client: MqttClient,
  topic: string,
  payload: string,
  timeoutMs: number
): Promise<void> {
  await new Promise<void>((resolve, reject) => {
    let settled = false;
    const timeout = setTimeout(() => {
      if (settled) return;
      settled = true;
      reject(new RouteServiceUnavailableError("MQTT publish timed out"));
    }, timeoutMs);

    client.publish(topic, payload, { qos: 2, retain: false }, (error) => {
      if (settled) return;
      settled = true;
      clearTimeout(timeout);
      if (error) reject(error);
      else resolve();
    });
  });
}

async function publishQos0WithTimeout(
  client: MqttClient,
  topic: string,
  payload: string,
  timeoutMs: number
): Promise<void> {
  await new Promise<void>((resolve, reject) => {
    let settled = false;
    const timeout = setTimeout(() => {
      if (settled) return;
      settled = true;
      reject(new RouteServiceUnavailableError("MQTT latency probe timed out"));
    }, timeoutMs);

    client.publish(topic, payload, { qos: 0, retain: false }, (error) => {
      if (settled) return;
      settled = true;
      clearTimeout(timeout);
      if (error) reject(error);
      else resolve();
    });
  });
}

function handleMessage(
  topic: string,
  payload: string,
  topics: ReturnType<typeof buildTopics>,
  handlers: RouteServiceHandlers,
  logger: Logger
): void {
  let parsedJson: unknown;
  try {
    parsedJson = JSON.parse(payload);
  } catch {
    logger.warn("Discarded invalid MQTT JSON", { topic });
    return;
  }

  if (topic === topics.configAck || topic === topics.controlAck) {
    const parsed = RouteCommandAckSchema.safeParse(parsedJson);
    if (!parsed.success) {
      logger.warn("Discarded invalid route_service command ACK", { topic });
      return;
    }
    const expectedType = topic === topics.configAck ? "config" : "control";
    if (parsed.data.command_type !== expectedType) {
      logger.warn("Discarded command ACK from the wrong topic", { topic });
      return;
    }
    handlers.onCommandAck(parsed.data);
    return;
  }

  const px4TopicSuffix = "/px4/realtime/v1";
  if (topic.startsWith(topics.px4RealtimePrefix) &&
      topic.endsWith(px4TopicSuffix)) {
    const parsed = Px4RealtimeFrameSchema.safeParse(parsedJson);
    if (!parsed.success) {
      logger.warn("Discarded invalid PX4 realtime frame", { topic });
      return;
    }
    const topicDeviceId = topic.slice(
      topics.px4RealtimePrefix.length,
      -px4TopicSuffix.length
    );
    if (topicDeviceId !== parsed.data.device_id) {
      logger.warn("Discarded PX4 realtime frame with mismatched device_id", { topic });
      return;
    }
    handlers.onPx4Realtime(parsed.data);
    return;
  }

  const px4LatencyAckSuffix = "/px4/latency/ack/v1";
  if (topic.startsWith(topics.px4RealtimePrefix) &&
      topic.endsWith(px4LatencyAckSuffix)) {
    const parsed = Px4LatencyAckMessageSchema.safeParse(parsedJson);
    if (!parsed.success) {
      logger.warn("Discarded invalid PX4 latency ACK", { topic });
      return;
    }
    const topicDeviceId = topic.slice(
      topics.px4RealtimePrefix.length,
      -px4LatencyAckSuffix.length
    );
    if (topicDeviceId !== parsed.data.device_id) {
      logger.warn("Discarded PX4 latency ACK with mismatched device_id", { topic });
      return;
    }
    handlers.onPx4LatencyAck(parsed.data);
    return;
  }

  if (topic.startsWith(topics.statePrefix) && topic.endsWith("/state")) {
    const parsed = RouteDeviceStateMessageSchema.safeParse(parsedJson);
    if (!parsed.success) {
      logger.warn("Discarded invalid route_service device state", { topic });
      return;
    }
    const topicVendorId = topic.slice(topics.statePrefix.length, -"/state".length);
    if (topicVendorId !== parsed.data.vendor_id) {
      logger.warn("Discarded device state with mismatched vendor_id", { topic });
      return;
    }
    handlers.onDeviceState(parsed.data);
  }
}
