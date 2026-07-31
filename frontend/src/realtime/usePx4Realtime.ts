import { useEffect, useMemo, useRef, useState } from "react";
import {
  BackendWebSocketEventSchema,
  type JsonValue,
  type Px4RealtimeEvent
} from "@cns/backend-protocol";

import { postPx4LatencyProbe } from "../api/client.js";
import { createUuidV4 } from "../utils/uuid.js";

export type Px4RealtimeStats = {
  framesPerSecond: number;
  receivedFrames: number;
  droppedFrames: number;
  roundTripLatencyMs: number | null;
  roundTripP50Ms: number | null;
  roundTripP95Ms: number | null;
  latencyJitterMs: number | null;
  latencySamples: number;
  latencyTimeouts: number;
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
  roundTripLatencyMs: null,
  roundTripP50Ms: null,
  roundTripP95Ms: null,
  latencyJitterMs: null,
  latencySamples: 0,
  latencyTimeouts: 0,
  lastFrameAt: null
};

const PROBE_INTERVAL_MS = 2000;
const PROBE_TIMEOUT_MS = 5000;
const MAX_LATENCY_SAMPLES = 30;

type PendingProbe = {
  startedAt: number;
  timeout: number;
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
    let probeTimer: number | undefined;
    let sessionId: string | null = null;
    const pendingProbes = new Map<string, PendingProbe>();
    const latencySamples: number[] = [];

    setEvent(null);
    setStats(EMPTY_STATS);
    pendingEvent.current = null;
    lastSequence.current = null;
    recentArrivals.current = [];

    const clearPendingProbe = (probeId: string): PendingProbe | null => {
      const pending = pendingProbes.get(probeId);
      if (!pending) return null;
      window.clearTimeout(pending.timeout);
      pendingProbes.delete(probeId);
      return pending;
    };

    const registerLatency = (latencyMs: number) => {
      const rounded = Math.max(0, Math.round(latencyMs));
      latencySamples.push(rounded);
      if (latencySamples.length > MAX_LATENCY_SAMPLES) latencySamples.shift();
      const sorted = [...latencySamples].sort((left, right) => left - right);
      const p50 = percentile(sorted, 0.5);
      const p95 = percentile(sorted, 0.95);
      setStats((current) => ({
        ...current,
        roundTripLatencyMs: rounded,
        roundTripP50Ms: p50,
        roundTripP95Ms: p95,
        latencyJitterMs: p50 === null || p95 === null ? null : Math.max(0, p95 - p50),
        latencySamples: latencySamples.length
      }));
    };

    const sendProbe = () => {
      if (closed || !sessionId || pendingProbes.size > 0) return;
      const probeId = createUuidV4();
      const startedAt = performance.now();
      const timeout = window.setTimeout(() => {
        pendingProbes.delete(probeId);
        setStats((current) => ({
          ...current,
          latencyTimeouts: current.latencyTimeouts + 1
        }));
      }, PROBE_TIMEOUT_MS);
      pendingProbes.set(probeId, { startedAt, timeout });

      void postPx4LatencyProbe(deviceId, sessionId, probeId).catch(() => {
        if (!clearPendingProbe(probeId)) return;
        setStats((current) => ({
          ...current,
          latencyTimeouts: current.latencyTimeouts + 1
        }));
      });
    };

    const startProbes = () => {
      if (probeTimer !== undefined) return;
      window.setTimeout(sendProbe, 200);
      probeTimer = window.setInterval(sendProbe, PROBE_INTERVAL_MS);
    };

    const updateFrameStats = (next: Px4RealtimeEvent) => {
      const now = Date.now();
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
        ...current,
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
        if (!parsed.success) return;

        if (parsed.data.type === "session.ready") {
          sessionId = parsed.data.session_id;
          startProbes();
          return;
        }

        if (parsed.data.type === "px4.latency_ack" &&
            parsed.data.device_id === deviceId &&
            parsed.data.session_id === sessionId) {
          const pending = clearPendingProbe(parsed.data.probe_id);
          if (pending) registerLatency(performance.now() - pending.startedAt);
          return;
        }

        if (parsed.data.type !== "px4.realtime" ||
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
        sessionId = null;
        pendingEvent.current = null;
        setEvent(null);
        if (probeTimer !== undefined) {
          window.clearInterval(probeTimer);
          probeTimer = undefined;
        }
        for (const probeId of pendingProbes.keys()) clearPendingProbe(probeId);
        retryTimer = window.setTimeout(connect, 1200);
      });
    };

    connect();
    return () => {
      closed = true;
      if (retryTimer) window.clearTimeout(retryTimer);
      if (staleTimer) window.clearTimeout(staleTimer);
      if (probeTimer !== undefined) window.clearInterval(probeTimer);
      for (const pending of pendingProbes.values()) window.clearTimeout(pending.timeout);
      pendingProbes.clear();
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

function percentile(sortedValues: number[], ratio: number): number | null {
  if (sortedValues.length === 0) return null;
  const index = Math.min(
    sortedValues.length - 1,
    Math.max(0, Math.ceil(sortedValues.length * ratio) - 1)
  );
  return sortedValues[index];
}
