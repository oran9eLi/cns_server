import { useEffect, useMemo, useState } from "react";
import type { QueryClient } from "@tanstack/react-query";
import {
  BackendWebSocketEventSchema,
  type CommandUpdatedEvent,
  type DeviceDetailResponse,
  type DeviceListResponse
} from "@cns/backend-protocol";

export type RealtimeState = {
  connected: boolean;
  sessionId: string | null;
  lastEventAt: string | null;
  commandEvents: CommandUpdatedEvent[];
};

export function useRealtime(queryClient: QueryClient): RealtimeState {
  const [connected, setConnected] = useState(false);
  const [sessionId, setSessionId] = useState<string | null>(null);
  const [lastEventAt, setLastEventAt] = useState<string | null>(null);
  const [commandEvents, setCommandEvents] = useState<CommandUpdatedEvent[]>([]);

  useEffect(() => {
    let closed = false;
    let socket: WebSocket | null = null;
    let retryTimer: number | undefined;

    const connect = () => {
      const protocol = window.location.protocol === "https:" ? "wss" : "ws";
      socket = new WebSocket(`${protocol}://${window.location.host}/ws`);

      socket.addEventListener("open", () => {
        if (!closed) setConnected(true);
      });

      socket.addEventListener("message", (message) => {
        const parsed = BackendWebSocketEventSchema.safeParse(JSON.parse(String(message.data)));
        if (!parsed.success) return;

        const event = parsed.data;
        setLastEventAt(new Date().toISOString());

        if (event.type === "session.ready") {
          setSessionId(event.session_id);
          return;
        }

        if (event.type === "device.state") {
          queryClient.setQueriesData<DeviceListResponse>({ queryKey: ["devices"] }, (current) => {
            if (!current) return current;
            return {
              ...current,
              items: current.items.map((item) =>
                item.vendor_id === event.vendor_id
                  ? {
                      ...item,
                      status: event.status,
                      last_seen_at: event.last_seen_at,
                      telemetry_received_at: event.telemetry_received_at,
                      degraded: event.degraded
                    }
                  : item
              )
            };
          });

          queryClient.setQueryData<DeviceDetailResponse>(["device", event.vendor_id], (current) => {
            if (!current) return current;
            return {
              ...current,
              item: {
                ...current.item,
                status: event.status,
                last_seen_at: event.last_seen_at,
                telemetry_received_at: event.telemetry_received_at,
                degraded: event.degraded,
                latest_telemetry: event.latest_telemetry
              }
            };
          });
          return;
        }

        setCommandEvents((events) => [event, ...events].slice(0, 16));
      });

      socket.addEventListener("close", () => {
        if (closed) return;
        setConnected(false);
        setSessionId(null);
        retryTimer = window.setTimeout(connect, 1200);
      });
    };

    connect();

    return () => {
      closed = true;
      if (retryTimer) window.clearTimeout(retryTimer);
      socket?.close();
    };
  }, [queryClient]);

  return useMemo(
    () => ({ connected, sessionId, lastEventAt, commandEvents }),
    [connected, sessionId, lastEventAt, commandEvents]
  );
}
