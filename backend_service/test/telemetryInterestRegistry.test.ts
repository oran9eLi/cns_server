import { describe, expect, it } from "vitest";

import { TelemetryInterestRegistry } from "../src/realtime/telemetryInterestRegistry.js";

const deviceOne = "CNS00000000000000001";
const deviceTwo = "CNS00000000000000002";

describe("WebSocket 会话遥测兴趣引用", () => {
  it("只在首个会话进入和最后一个会话离开时通知", () => {
    const changes: Array<[string, boolean]> = [];
    const registry = new TelemetryInterestRegistry(
      (deviceId, interested) => changes.push([deviceId, interested])
    );

    registry.subscribe("s1", deviceOne);
    registry.subscribe("s1", deviceOne);
    registry.subscribe("s2", deviceOne);
    registry.unsubscribe("s1", deviceOne);
    registry.removeSession("s2");

    expect(changes).toEqual([
      [deviceOne, true],
      [deviceOne, false]
    ]);
  });

  it("断开会话会清理其所有设备引用", () => {
    const registry = new TelemetryInterestRegistry(() => undefined);
    registry.subscribe("s1", deviceOne);
    registry.subscribe("s1", deviceTwo);

    registry.removeSession("s1");

    expect(registry.isInterested("s1", deviceOne)).toBe(false);
    expect(registry.isInterested("s1", deviceTwo)).toBe(false);
  });
});
