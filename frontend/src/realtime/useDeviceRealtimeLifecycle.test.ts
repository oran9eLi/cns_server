import { beforeEach, describe, expect, it, vi } from "vitest";

const react = vi.hoisted(() => ({
  cleanup: undefined as (() => void) | undefined,
  setters: [] as Array<ReturnType<typeof vi.fn>>
}));

vi.mock("react", () => ({
  useEffect: (effect: () => void | (() => void)) => {
    react.cleanup = effect() ?? undefined;
  },
  useMemo: (factory: () => unknown) => factory(),
  useRef: (initial: unknown) => ({ current: initial }),
  useState: (initial: unknown) => {
    const setter = vi.fn();
    react.setters.push(setter);
    return [initial, setter];
  }
}));

import { useDeviceRealtime } from "./useDeviceRealtime.js";

class FakeWebSocket {
  static readonly OPEN = 1;
  readonly listeners = new Map<string, Array<(event: { data?: string }) => void>>();
  readyState = FakeWebSocket.OPEN;
  send = vi.fn();
  close = vi.fn();

  addEventListener(event: string, listener: (payload: { data?: string }) => void) {
    const listeners = this.listeners.get(event) ?? [];
    listeners.push(listener);
    this.listeners.set(event, listeners);
  }

  emit(event: string, payload: { data?: string } = {}) {
    for (const listener of this.listeners.get(event) ?? []) listener(payload);
  }
}

describe("useDeviceRealtime 生命周期", () => {
  beforeEach(() => {
    react.cleanup = undefined;
    react.setters = [];
  });

  it("清理后忽略旧 WebSocket 已排队的消息", () => {
    const sockets: FakeWebSocket[] = [];
    const setTimeout = vi.fn(() => 1);
    const requestAnimationFrame = vi.fn(() => 1);
    vi.stubGlobal("WebSocket", class extends FakeWebSocket {
      constructor() {
        super();
        sockets.push(this);
      }
    });
    vi.stubGlobal("window", {
      location: { protocol: "http:", host: "localhost" },
      setTimeout,
      clearTimeout: vi.fn(),
      requestAnimationFrame,
      cancelAnimationFrame: vi.fn()
    });

    useDeviceRealtime("CNS00000000000000001", null, true);
    const socket = sockets[0];
    expect(socket).toBeDefined();
    react.cleanup?.();
    const setterCalls = react.setters.reduce(
      (total, setter) => total + setter.mock.calls.length,
      0
    );

    socket.emit("message", {
      data: JSON.stringify({
        type: "telemetry.realtime",
        schema_version: 1,
        device_id: "CNS00000000000000001",
        sequence: 1,
        sent_at: "2026-08-05T01:02:03.123Z",
        server_received_at: "2026-08-05T01:02:03.140Z",
        telemetry: { motor: { pwm_us: [1000, 1000, 1000, 1000] } }
      })
    });

    expect(react.setters.reduce(
      (total, setter) => total + setter.mock.calls.length,
      0
    )).toBe(setterCalls);
    expect(setTimeout).not.toHaveBeenCalled();
    expect(requestAnimationFrame).not.toHaveBeenCalled();
  });
});
