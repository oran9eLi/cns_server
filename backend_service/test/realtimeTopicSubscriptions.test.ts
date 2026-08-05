import { afterEach, describe, expect, it, vi } from "vitest";

import { RealtimeTopicSubscriptions } from "../src/route/realtimeTopicSubscriptions.js";

const deviceOne = "CNS00000000000000001";
const deviceTwo = "CNS00000000000000002";

describe("实时遥测 MQTT 订阅收敛", () => {
  afterEach(() => {
    vi.useRealTimers();
  });

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

  it("旧连接订阅迟到完成后仍为新连接重新订阅", async () => {
    const subscriptions: string[] = [];
    let releaseFirst = () => undefined;
    const firstBlocked = new Promise<void>((resolve) => {
      releaseFirst = resolve;
    });
    const controller = new RealtimeTopicSubscriptions({
      topicFor: (id) => `cns_rpi/${id}/telemetry/realtime/v1`,
      subscribe: async (topic) => {
        subscriptions.push(topic);
        if (subscriptions.length === 1) await firstBlocked;
      },
      unsubscribe: async () => undefined,
      onError: () => undefined
    });

    controller.setConnected(true);
    const settling = controller.setDesired(deviceOne, true);
    await Promise.resolve();
    expect(subscriptions).toHaveLength(1);

    controller.setConnected(false);
    controller.setConnected(true);
    releaseFirst();
    await settling;

    expect(subscriptions).toEqual([
      `cns_rpi/${deviceOne}/telemetry/realtime/v1`,
      `cns_rpi/${deviceOne}/telemetry/realtime/v1`
    ]);
    expect(controller.appliedDeviceIds()).toEqual([deviceOne]);
  });

  it("订阅回调不返回时在超时后释放收敛循环", async () => {
    vi.useFakeTimers();
    const errors: string[] = [];
    const controller = new RealtimeTopicSubscriptions({
      topicFor: (id) => `cns_rpi/${id}/telemetry/realtime/v1`,
      subscribe: async () => new Promise<void>(() => undefined),
      unsubscribe: async () => undefined,
      operationTimeoutMs: 50,
      onError: (operation) => errors.push(operation)
    });

    controller.setConnected(true);
    const settling = controller.setDesired(deviceOne, true);
    await vi.advanceTimersByTimeAsync(50);
    await settling;

    expect(errors).toEqual(["subscribe"]);
    expect(controller.appliedDeviceIds()).toEqual([]);
  });

  it("旧连接订阅永久阻塞时超时后仍恢复新连接订阅", async () => {
    vi.useFakeTimers();
    let attempts = 0;
    const controller = new RealtimeTopicSubscriptions({
      topicFor: (id) => `cns_rpi/${id}/telemetry/realtime/v1`,
      subscribe: async () => {
        attempts += 1;
        if (attempts === 1) await new Promise<void>(() => undefined);
      },
      unsubscribe: async () => undefined,
      operationTimeoutMs: 50,
      onError: () => undefined
    });

    controller.setConnected(true);
    const settling = controller.setDesired(deviceOne, true);
    await Promise.resolve();
    controller.setConnected(false);
    controller.setConnected(true);
    await vi.advanceTimersByTimeAsync(50);
    await settling;

    expect(attempts).toBe(2);
    expect(controller.appliedDeviceIds()).toEqual([deviceOne]);
  });
});
