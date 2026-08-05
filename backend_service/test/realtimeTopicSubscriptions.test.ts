import { describe, expect, it } from "vitest";

import { RealtimeTopicSubscriptions } from "../src/route/realtimeTopicSubscriptions.js";

const deviceOne = "CNS00000000000000001";
const deviceTwo = "CNS00000000000000002";

describe("实时遥测 MQTT 订阅收敛", () => {
  it("交错订阅与退订最终收敛到目标集合", async () => {
    const calls: string[] = [];
    let releaseSubscribe = () => undefined;
    const subscribeBlocked = new Promise<void>((resolve) => {
      releaseSubscribe = resolve;
    });
    const controller = new RealtimeTopicSubscriptions({
      topicFor: (id) => `cns_rpi/${id}/telemetry/realtime/v1`,
      subscribe: async (topic) => {
        calls.push(`sub:${topic}`);
        await subscribeBlocked;
      },
      unsubscribe: async (topic) => {
        calls.push(`unsub:${topic}`);
      },
      onError: () => undefined
    });

    controller.setConnected(true);
    const first = controller.setDesired(deviceOne, true);
    await Promise.resolve();
    const last = controller.setDesired(deviceOne, false);
    releaseSubscribe();
    await Promise.all([first, last, controller.settle()]);

    expect(controller.desiredDeviceIds()).toEqual([]);
    expect(controller.appliedDeviceIds()).toEqual([]);
    expect(calls).toEqual([
      `sub:cns_rpi/${deviceOne}/telemetry/realtime/v1`,
      `unsub:cns_rpi/${deviceOne}/telemetry/realtime/v1`
    ]);
  });

  it("重连只恢复仍有兴趣的设备", async () => {
    const subscriptions: string[] = [];
    const controller = new RealtimeTopicSubscriptions({
      topicFor: (id) => `cns_rpi/${id}/telemetry/realtime/v1`,
      subscribe: async (topic) => {
        subscriptions.push(topic);
      },
      unsubscribe: async () => undefined,
      onError: () => undefined
    });

    controller.setConnected(true);
    await controller.setDesired(deviceOne, true);
    await controller.setDesired(deviceTwo, true);
    await controller.setDesired(deviceTwo, false);
    controller.setConnected(false);
    controller.setConnected(true);
    await controller.settle();

    expect(controller.desiredDeviceIds()).toEqual([deviceOne]);
    expect(controller.appliedDeviceIds()).toEqual([deviceOne]);
    expect(subscriptions.filter((topic) => topic.includes(deviceOne))).toHaveLength(2);
    expect(subscriptions.filter((topic) => topic.includes(deviceTwo))).toHaveLength(1);
  });
});
