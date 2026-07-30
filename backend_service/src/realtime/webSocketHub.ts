import websocket from "@fastify/websocket";
import type { FastifyInstance } from "fastify";
import { randomUUID } from "node:crypto";

import {
  FrontendWebSocketMessageSchema,
  SCHEMA_VERSION,
  SessionReadyEventSchema,
  WEBSOCKET_PATH,
  type BackendWebSocketEvent,
  type Px4RealtimeEvent
} from "@cns/backend-protocol";

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
  broadcastPx4(deviceId: string, event: Px4RealtimeEvent): void;
}

export function createWebSocketHub(): WebSocketHub {
  const sessions = new Map<string, {
    socket: WebSocketPeer;
    px4DeviceIds: Set<string>;
  }>();

  return {
    async register(app) {
      await app.register(websocket);

      app.get(WEBSOCKET_PATH, { websocket: true }, (connection) => {
        const socket = getSocket(connection as WebSocketConnection);
        const sessionId = `session_${randomUUID()}`;
        const session = {
          socket,
          px4DeviceIds: new Set<string>()
        };
        sessions.set(sessionId, session);

        send(socket, SessionReadyEventSchema.parse({
          type: "session.ready",
          schema_version: SCHEMA_VERSION,
          session_id: sessionId,
          server_time: new Date().toISOString()
        }));

        socket.on?.("close", () => {
          sessions.delete(sessionId);
        });
        socket.on?.("message", (payload) => {
          const message = parseClientMessage(payload);
          if (!message) return;
          if (message.type === "px4.subscribe") {
            session.px4DeviceIds.add(message.device_id);
          } else {
            session.px4DeviceIds.delete(message.device_id);
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
        send(session.socket, event);
      }
    },
    broadcast(event) {
      for (const session of sessions.values()) {
        send(session.socket, event);
      }
    },
    broadcastPx4(deviceId, event) {
      for (const session of sessions.values()) {
        if (!session.px4DeviceIds.has(deviceId)) continue;
        // 实时流不排队：浏览器处理不过来时丢弃旧帧，避免延迟越积越高。
        if ((session.socket.bufferedAmount ?? 0) > 256 * 1024) continue;
        send(session.socket, event);
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
