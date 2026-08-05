import { useEffect, useMemo, useRef, useState } from "react";
import type { QueryClient } from "@tanstack/react-query";
import {
  BackendWebSocketEventSchema,
  type CommandUpdatedEvent,
  type DeviceDetailResponse,
  type DeviceListResponse,
  type DeviceStateEvent
} from "@cns/backend-protocol";

export type RealtimeState = {
  connected: boolean;
  sessionId: string | null;
  lastEventAt: string | null;
  commandEvents: CommandUpdatedEvent[];
};

export function mergeDeviceListState(
  current: DeviceListResponse,
  event: DeviceStateEvent
): DeviceListResponse {
  return {
    ...current,
    items: current.items.map((item) =>
      item.device_id === event.device_id
        ? {
            ...item,
            device_id: event.device_id,
            device_type: event.device_type,
            school_name: event.school_name,
            dcdw_label: event.dcdw_label,
            capabilities: event.capabilities,
            product: event.product,
            version: event.version,
            status: event.status,
            last_seen_at: event.last_seen_at,
            telemetry_received_at: event.telemetry_received_at,
            degraded: event.degraded
          }
        : item
    )
  };
}

export function mergeDeviceDetailState(
  current: DeviceDetailResponse,
  event: DeviceStateEvent
): DeviceDetailResponse {
  return {
    ...current,
    item: {
      ...current.item,
      device_id: event.device_id,
      device_type: event.device_type,
      school_name: event.school_name,
      dcdw_label: event.dcdw_label,
      capabilities: event.capabilities,
      product: event.product,
      version: event.version,
      status: event.status,
      last_seen_at: event.last_seen_at,
      telemetry_received_at: event.telemetry_received_at,
      degraded: event.degraded,
      latest_telemetry: event.latest_telemetry
    }
  };
}

export function useRealtime(queryClient: QueryClient): RealtimeState {
  const [connected, setConnected] = useState(false);
  const [sessionId, setSessionId] = useState<string | null>(null);
  const [lastEventAt, setLastEventAt] = useState<string | null>(null);
  const [commandEvents, setCommandEvents] = useState<CommandUpdatedEvent[]>([]);
  const latestDeviceEventAt = useRef(new Map<string, string>());

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
        let payload: unknown;
        try {
          payload = JSON.parse(String(message.data));
        } catch {
          return;
        }
        const parsed = BackendWebSocketEventSchema.safeParse(payload);
        if (!parsed.success) return;

        const event = parsed.data;

        if (event.type === "session.ready") {
          setSessionId(event.session_id);
          return;
        }

        if (event.type === "device.state") {
          const previousEventAt = latestDeviceEventAt.current.get(event.device_id);
          if (previousEventAt && previousEventAt >= event.event_at) return;
          latestDeviceEventAt.current.set(event.device_id, event.event_at);
          setLastEventAt(event.event_at);

          queryClient.setQueriesData<DeviceListResponse>({ queryKey: ["devices"] }, (current) => {
            if (!current) return current;
            return mergeDeviceListState(current, event);
          });

          queryClient.setQueryData<DeviceDetailResponse>(["device", event.device_id], (current) => {
            if (!current) return current;
            return mergeDeviceDetailState(current, event);
          });
          return;
        }

        // 高频帧只由设备详情页的按设备连接消费，不能写入全局查询缓存。
        if (event.type === "telemetry.realtime") return;

        setLastEventAt(event.updated_at);
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
