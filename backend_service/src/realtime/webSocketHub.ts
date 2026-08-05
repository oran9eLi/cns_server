import websocket from "@fastify/websocket";
import type { FastifyInstance } from "fastify";
import { randomUUID } from "node:crypto";

import {
  FrontendWebSocketMessageSchema,
  SCHEMA_VERSION,
  SessionReadyEventSchema,
  WEBSOCKET_PATH,
  type BackendWebSocketEvent,
  type DeviceRealtimeEvent
} from "@cns/backend-protocol";
import {
  TelemetryInterestRegistry,
  type TelemetryInterestChanged
} from "./telemetryInterestRegistry.js";

type WebSocketPeer = {
  bufferedAmount?: number;
  send(payload: string): void;
  on?(event: "close", handler: () => void): void;
  on?(event: "message", handler: (payload: unknown) => void): void;
};

type WebSocketConnection = WebSocketPeer & {
  socket?: WebSocketPeer;
};

export interface WebSocketHub {
  register(app: FastifyInstance): Promise<void>;
  hasSession(sessionId: string): boolean;
  sendToSession(sessionId: string, event: BackendWebSocketEvent): void;
  broadcast(event: BackendWebSocketEvent): void;
  broadcastTelemetry(deviceId: string, event: DeviceRealtimeEvent): void;
}

export function createWebSocketHub(
  onTelemetryInterestChanged: TelemetryInterestChanged = () => undefined
): WebSocketHub {
  const sessions = new Map<string, WebSocketPeer>();
  const interests = new TelemetryInterestRegistry(onTelemetryInterestChanged);

  return {
    async register(app) {
      await app.register(websocket);

      app.get(WEBSOCKET_PATH, { websocket: true }, (connection) => {
        const socket = getSocket(connection as WebSocketConnection);
        const sessionId = `session_${randomUUID()}`;
        sessions.set(sessionId, socket);

        send(socket, SessionReadyEventSchema.parse({
          type: "session.ready",
          schema_version: SCHEMA_VERSION,
          session_id: sessionId,
          server_time: new Date().toISOString()
        }));

        socket.on?.("close", () => {
          interests.removeSession(sessionId);
          sessions.delete(sessionId);
        });
        socket.on?.("message", (payload) => {
          const message = parseClientMessage(payload);
          if (!message) return;
          if (message.type === "telemetry.subscribe") {
            interests.subscribe(sessionId, message.device_id);
          } else {
            interests.unsubscribe(sessionId, message.device_id);
          }
        });
      });
    },
    hasSession(sessionId) {
      return sessions.has(sessionId);
    },
    sendToSession(sessionId, event) {
      const session = sessions.get(sessionId);
      if (session) {
        send(session, event);
      }
    },
    broadcast(event) {
      for (const socket of sessions.values()) {
        send(socket, event);
      }
    },
    broadcastTelemetry(deviceId, event) {
      for (const [sessionId, socket] of sessions) {
        if (!interests.isInterested(sessionId, deviceId)) continue;
        // 实时流不排队：浏览器处理不过来时丢弃旧帧，避免延迟越积越高。
        if ((socket.bufferedAmount ?? 0) > 256 * 1024) continue;
        send(socket, event);
      }
    }
  };
}

function send(socket: WebSocketPeer, event: BackendWebSocketEvent): void {
  socket.send(JSON.stringify(event));
}

function getSocket(connection: WebSocketConnection): WebSocketPeer {
  return connection.socket ?? connection;
}

function parseClientMessage(payload: unknown) {
  let json: unknown;
  try {
    json = JSON.parse(String(payload));
  } catch {
    return null;
  }
  const parsed = FrontendWebSocketMessageSchema.safeParse(json);
  return parsed.success ? parsed.data : null;
}
