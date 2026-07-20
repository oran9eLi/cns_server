import websocket from "@fastify/websocket";
import type { FastifyInstance } from "fastify";
import { randomUUID } from "node:crypto";

import {
  SCHEMA_VERSION,
  SessionReadyEventSchema,
  WEBSOCKET_PATH,
  type BackendWebSocketEvent
} from "@cns/backend-protocol";

type WebSocketPeer = {
  send(payload: string): void;
  on?(event: "close", handler: () => void): void;
};

type WebSocketConnection = WebSocketPeer & {
  socket?: WebSocketPeer;
};

export interface WebSocketHub {
  register(app: FastifyInstance): Promise<void>;
  hasSession(sessionId: string): boolean;
  sendToSession(sessionId: string, event: BackendWebSocketEvent): void;
  broadcast(event: BackendWebSocketEvent): void;
}

export function createWebSocketHub(): WebSocketHub {
  const sessions = new Map<string, WebSocketPeer>();

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
          sessions.delete(sessionId);
        });
      });
    },
    hasSession(sessionId) {
      return sessions.has(sessionId);
    },
    sendToSession(sessionId, event) {
      const socket = sessions.get(sessionId);
      if (socket) {
        send(socket, event);
      }
    },
    broadcast(event) {
      for (const socket of sessions.values()) {
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
