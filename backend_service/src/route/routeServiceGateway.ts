import { connect, type IClientOptions, type MqttClient } from "mqtt";

import type {
  DependencyStatus,
  DeviceCommandRequest,
  DeviceRealtimeFrame
} from "@cns/backend-protocol";
import {
  DeviceRealtimeFrameSchema
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
import { RealtimeTopicSubscriptions } from "./realtimeTopicSubscriptions.js";

type MqttConfig = NonNullable<AppConfig["mqtt"]>;

export interface RouteServiceGateway {
  start(): Promise<void>;
  setRealtimeInterest(deviceId: string, interested: boolean): Promise<void>;
  publishCommand(deviceId: string, request: DeviceCommandRequest): Promise<void>;
  dependencyStatus(): Promise<DependencyStatus>;
  close(): Promise<void>;
}

export type RouteServiceHandlers = {
  onDeviceState(message: RouteDeviceStateMessage): void;
  onCommandAck(message: RouteCommandAck): void;
  onDeviceRealtime(message: DeviceRealtimeFrame): void;
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
    async setRealtimeInterest() {
      return undefined;
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
  const subscriptions = new RealtimeTopicSubscriptions({
    topicFor: topics.realtimeTopic,
    subscribe: async (topic) => {
      if (!client) throw new RouteServiceUnavailableError("MQTT is unavailable");
      await subscribeQos0(client, topic);
    },
    unsubscribe: async (topic) => {
      if (!client) throw new RouteServiceUnavailableError("MQTT is unavailable");
      await unsubscribe(client, topic);
    },
    operationTimeoutMs: config.connect_timeout_ms,
    onError(operation, topic, error) {
      logger.warn("实时遥测 MQTT 订阅收敛失败", {
        operation,
        topic,
        error: error instanceof Error ? error.message : String(error)
      });
    }
  });

  return {
    async start() {
      if (client) return;
      client = connect(`${config.protocol}://${config.host}:${config.port}`, clientOptions(config));
      client.on("message", (topic, payload) => {
        handleMessage(topic, payload.toString("utf8"), topics, handlers, logger);
      });
      client.on("close", () => {
        status = "unavailable";
        subscriptions.setConnected(false);
      });
      client.on("offline", () => {
        status = "unavailable";
        subscriptions.setConnected(false);
      });
      client.on("error", (error) => {
        logger.warn("MQTT connection error", { error: error.message });
      });

      try {
        await waitUntilSubscribed(client, config, topics, () => {
          status = "ready";
          subscriptions.setConnected(true);
        });
      } catch (error) {
        const failedClient = client;
        client = null;
        status = "unavailable";
        await new Promise<void>((resolve) => failedClient.end(true, {}, () => resolve()));
        throw error;
      }
    },
    async setRealtimeInterest(deviceId, interested) {
      await subscriptions.setDesired(deviceId, interested);
    },
    async publishCommand(deviceId, request) {
      if (!client || status !== "ready") {
        throw new RouteServiceUnavailableError("MQTT is unavailable");
      }

      const topic = request.type === "config" ? topics.configRequest : topics.controlRequest;
      const payload = JSON.stringify(buildRouteCommandRequest(deviceId, request));
      await publishWithTimeout(client, topic, payload, config.publish_timeout_ms);
    },
    async dependencyStatus() {
      return status;
    },
    async close() {
      const activeClient = client;
      client = null;
      status = "unavailable";
      subscriptions.setConnected(false);
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
    devicePrefix: `${config.topic_namespace}/`,
    realtimeTopic: (deviceId: string) =>
      `${config.topic_namespace}/${deviceId}/telemetry/realtime/v1`,
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

async function subscribeQos0(
  client: MqttClient,
  topic: string
): Promise<void> {
  await new Promise<void>((resolve, reject) => {
    client.subscribe(topic, { qos: 0 }, (error) => {
      if (error) reject(error);
      else resolve();
    });
  });
}

async function unsubscribe(client: MqttClient, topic: string): Promise<void> {
  await new Promise<void>((resolve, reject) => {
    client.unsubscribe(topic, (error) => {
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

  const realtimeTopicSuffix = "/telemetry/realtime/v1";
  if (topic.startsWith(topics.devicePrefix) &&
      topic.endsWith(realtimeTopicSuffix)) {
    const parsed = DeviceRealtimeFrameSchema.safeParse(parsedJson);
    if (!parsed.success) {
      logger.warn("Discarded invalid device realtime frame", { topic });
      return;
    }
    const topicDeviceId = topic.slice(
      topics.devicePrefix.length,
      -realtimeTopicSuffix.length
    );
    if (topicDeviceId !== parsed.data.device_id) {
      logger.warn("Discarded device realtime frame with mismatched device_id", {
        topic
      });
      return;
    }
    handlers.onDeviceRealtime(parsed.data);
    return;
  }

  if (topic.startsWith(topics.statePrefix) && topic.endsWith("/state")) {
    const parsed = RouteDeviceStateMessageSchema.safeParse(parsedJson);
    if (!parsed.success) {
      logger.warn("Discarded invalid route_service device state", { topic });
      return;
    }
    const topicDeviceId = topic.slice(topics.statePrefix.length, -"/state".length);
    if (topicDeviceId !== parsed.data.device_id) {
      logger.warn("Discarded device state with mismatched device_id", { topic });
      return;
    }
    handlers.onDeviceState(parsed.data);
  }
}
