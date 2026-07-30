import { useEffect, useMemo, useRef, useState } from "react";
import {
  BackendWebSocketEventSchema,
  type JsonValue,
  type Px4RealtimeEvent
} from "@cns/backend-protocol";

export type Px4RealtimeStats = {
  framesPerSecond: number;
  receivedFrames: number;
  droppedFrames: number;
  uplinkLatencyMs: number | null;
  browserLatencyMs: number | null;
  endToEndLatencyMs: number | null;
  lastFrameAt: string | null;
};

export type Px4RealtimeState = {
  connected: boolean;
  usingFastPath: boolean;
  telemetry: JsonValue | null;
  stats: Px4RealtimeStats;
};

const EMPTY_STATS: Px4RealtimeStats = {
  framesPerSecond: 0,
  receivedFrames: 0,
  droppedFrames: 0,
  uplinkLatencyMs: null,
  browserLatencyMs: null,
  endToEndLatencyMs: null,
  lastFrameAt: null
};

export function usePx4Realtime(
  deviceId: string,
  fallbackTelemetry: JsonValue | null
): Px4RealtimeState {
  const [connected, setConnected] = useState(false);
  const [event, setEvent] = useState<Px4RealtimeEvent | null>(null);
  const [stats, setStats] = useState<Px4RealtimeStats>(EMPTY_STATS);
  const pendingEvent = useRef<Px4RealtimeEvent | null>(null);
  const animationFrame = useRef<number | null>(null);
  const lastSequence = useRef<number | null>(null);
  const recentArrivals = useRef<number[]>([]);

  useEffect(() => {
    let closed = false;
    let socket: WebSocket | null = null;
    let retryTimer: number | undefined;
    let staleTimer: number | undefined;

    setEvent(null);
    setStats(EMPTY_STATS);
    pendingEvent.current = null;
    lastSequence.current = null;
    recentArrivals.current = [];

    const connect = () => {
      const protocol = window.location.protocol === "https:" ? "wss" : "ws";
      socket = new WebSocket(`${protocol}://${window.location.host}/ws`);

      socket.addEventListener("open", () => {
        if (closed) return;
        setConnected(true);
        socket?.send(JSON.stringify({ type: "px4.subscribe", device_id: deviceId }));
      });

      socket.addEventListener("message", (message) => {
        let payload: unknown;
        try {
          payload = JSON.parse(String(message.data));
        } catch {
          return;
        }
        const parsed = BackendWebSocketEventSchema.safeParse(payload);
        if (!parsed.success || parsed.data.type !== "px4.realtime" ||
            parsed.data.device_id !== deviceId) {
          return;
        }

        const next = parsed.data;
        pendingEvent.current = next;
        updateStats(next);
        if (staleTimer) window.clearTimeout(staleTimer);
        staleTimer = window.setTimeout(() => {
          pendingEvent.current = null;
          setEvent(null);
        }, 2000);

        // 一次绘制周期内只渲染最新帧，网络突发时不会把陈旧帧排队到 UI。
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

    const updateStats = (next: Px4RealtimeEvent) => {
      const now = Date.now();
      const sentAt = Date.parse(next.sent_at);
      const serverAt = Date.parse(next.server_received_at);
      const previousSequence = lastSequence.current;
      const dropped = previousSequence !== null && next.sequence > previousSequence
        ? Math.max(0, next.sequence - previousSequence - 1)
        : 0;
      lastSequence.current = next.sequence;

      recentArrivals.current.push(now);
      recentArrivals.current = recentArrivals.current.filter((value) => now - value <= 2000);
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
        uplinkLatencyMs: finiteLatency(serverAt - sentAt),
        browserLatencyMs: finiteLatency(now - serverAt),
        endToEndLatencyMs: finiteLatency(now - sentAt),
        lastFrameAt: new Date(now).toISOString()
      }));
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
        socket.send(JSON.stringify({ type: "px4.unsubscribe", device_id: deviceId }));
      }
      socket?.close();
    };
  }, [deviceId]);

  const telemetry = useMemo(
    () => mergePx4Telemetry(fallbackTelemetry, event),
    [fallbackTelemetry, event]
  );

  return {
    connected,
    usingFastPath: event !== null,
    telemetry,
    stats
  };
}

export function mergePx4Telemetry(
  fallback: JsonValue | null,
  event: Px4RealtimeEvent | null
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

function finiteLatency(value: number): number | null {
  return Number.isFinite(value) && value >= -1000 && value <= 60_000
    ? Math.max(0, Math.round(value))
    : null;
}
