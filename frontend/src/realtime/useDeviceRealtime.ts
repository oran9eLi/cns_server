import { useEffect, useMemo, useRef, useState } from "react";
import {
  BackendWebSocketEventSchema,
  type DeviceRealtimeEvent,
  type JsonValue
} from "@cns/backend-protocol";

export type DeviceRealtimeState = {
  connected: boolean;
  usingRealtime: boolean;
  telemetry: JsonValue | null;
  framesPerSecond: number;
  receivedFrames: number;
  droppedFrames: number;
  lastFrameAt: string | null;
};

type DeviceRealtimeStats = Pick<
  DeviceRealtimeState,
  "framesPerSecond" | "receivedFrames" | "droppedFrames" | "lastFrameAt"
>;

const EMPTY_STATS: DeviceRealtimeStats = {
  framesPerSecond: 0,
  receivedFrames: 0,
  droppedFrames: 0,
  lastFrameAt: null
};

export function useDeviceRealtime(
  deviceId: string,
  fallbackTelemetry: JsonValue | null,
  enabled = true
): DeviceRealtimeState {
  const [connected, setConnected] = useState(false);
  const [event, setEvent] = useState<DeviceRealtimeEvent | null>(null);
  const [stats, setStats] = useState<DeviceRealtimeStats>(EMPTY_STATS);
  const pendingEvent = useRef<DeviceRealtimeEvent | null>(null);
  const animationFrame = useRef<number | null>(null);
  const lastSequence = useRef<number | null>(null);
  const recentArrivals = useRef<number[]>([]);

  useEffect(() => {
    let closed = false;
    let socket: WebSocket | null = null;
    let retryTimer: number | undefined;
    let staleTimer: number | undefined;

    setConnected(false);
    setEvent(null);
    setStats(EMPTY_STATS);
    pendingEvent.current = null;
    lastSequence.current = null;
    recentArrivals.current = [];

    if (!enabled) return undefined;

    const updateFrameStats = (next: DeviceRealtimeEvent) => {
      const now = Date.now();
      const previousSequence = lastSequence.current;
      const dropped = previousSequence !== null && next.sequence > previousSequence
        ? Math.max(0, next.sequence - previousSequence - 1)
        : 0;
      lastSequence.current = next.sequence;

      recentArrivals.current.push(now);
      recentArrivals.current = recentArrivals.current.filter(
        (value) => now - value <= 2000
      );
      const arrivals = recentArrivals.current;
      const sampleWindow = arrivals.length > 1
        ? Math.max(1, arrivals[arrivals.length - 1] - arrivals[0])
        : 1000;

      setStats((current) => ({
        framesPerSecond: arrivals.length > 1
          ? ((arrivals.length - 1) * 1000) / sampleWindow
          : arrivals.length,
        receivedFrames: current.receivedFrames + 1,
        droppedFrames: current.droppedFrames + dropped,
        lastFrameAt: new Date(now).toISOString()
      }));
    };

    const connect = () => {
      const protocol = window.location.protocol === "https:" ? "wss" : "ws";
      socket = new WebSocket(`${protocol}://${window.location.host}/ws`);

      socket.addEventListener("open", () => {
        if (closed) return;
        setConnected(true);
        socket?.send(JSON.stringify({
          type: "telemetry.subscribe",
          device_id: deviceId
        }));
      });

      socket.addEventListener("message", (message) => {
        let payload: unknown;
        try {
          payload = JSON.parse(String(message.data));
        } catch {
          return;
        }
        const parsed = BackendWebSocketEventSchema.safeParse(payload);
        if (!parsed.success ||
            parsed.data.type !== "telemetry.realtime" ||
            parsed.data.device_id !== deviceId) {
          return;
        }

        const next = parsed.data;
        pendingEvent.current = next;
        updateFrameStats(next);
        if (staleTimer) window.clearTimeout(staleTimer);
        staleTimer = window.setTimeout(() => {
          pendingEvent.current = null;
          setEvent(null);
        }, 2000);

        // 同一绘制周期只应用最后一帧，避免网络突发形成 UI 排队。
        if (animationFrame.current === null) {
          animationFrame.current = window.requestAnimationFrame(() => {
            animationFrame.current = null;
            if (pendingEvent.current) setEvent(pendingEvent.current);
          });
        }
      });

      socket.addEventListener("close", () => {
        if (closed) return;
        setConnected(false);
        pendingEvent.current = null;
        setEvent(null);
        retryTimer = window.setTimeout(connect, 1200);
      });
    };

    connect();
    return () => {
      closed = true;
      if (retryTimer) window.clearTimeout(retryTimer);
      if (staleTimer) window.clearTimeout(staleTimer);
      if (animationFrame.current !== null) {
        window.cancelAnimationFrame(animationFrame.current);
        animationFrame.current = null;
      }
      if (socket?.readyState === WebSocket.OPEN) {
        socket.send(JSON.stringify({
          type: "telemetry.unsubscribe",
          device_id: deviceId
        }));
      }
      socket?.close();
    };
  }, [deviceId, enabled]);

  const telemetry = useMemo(
    () => mergeDeviceTelemetry(fallbackTelemetry, event),
    [fallbackTelemetry, event]
  );

  return {
    connected,
    usingRealtime: event !== null,
    telemetry,
    ...stats
  };
}

export function mergeDeviceTelemetry(
  fallback: JsonValue | null,
  event: DeviceRealtimeEvent | null
): JsonValue | null {
  if (!event) return fallback;
  const fallbackObject = asObject(fallback);
  const fallbackRealtime = asObject(fallbackObject.telemetry);
  return {
    ...fallbackObject,
    sent_at: event.sent_at,
    telemetry: {
      ...fallbackRealtime,
      ...event.telemetry
    }
  };
}

function asObject(value: JsonValue | null | undefined): Record<string, JsonValue> {
  return value && typeof value === "object" && !Array.isArray(value) ? value : {};
}
