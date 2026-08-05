import { EventEmitter } from "node:events";

import { beforeEach, describe, expect, it, vi } from "vitest";

const mqtt = vi.hoisted(() => ({
  client: null as FakeMqttClient | null
}));

vi.mock("mqtt", () => ({
  connect: vi.fn(() => {
    mqtt.client = new FakeMqttClient();
    queueMicrotask(() => mqtt.client?.emit("connect"));
    return mqtt.client;
  })
}));

import { AppConfigSchema } from "../src/config/appConfig.js";
import type { Logger } from "../src/logging/logger.js";
import { createRouteServiceGateway } from "../src/route/routeServiceGateway.js";

const deviceId = "CNS00000000000000001";

class FakeMqttClient extends EventEmitter {
  readonly subscriptions: string[] = [];
  readonly unsubscriptions: string[] = [];

  subscribe(
    topics: string | Record<string, unknown>,
    optionsOrCallback: unknown,
    callback?: (error?: Error) => void
  ): this {
    if (typeof topics === "string") {
      this.subscriptions.push(topics);
      callback?.();
    } else {
      this.subscriptions.push(...Object.keys(topics));
      (optionsOrCallback as (error?: Error) => void)();
    }
    return this;
  }

  unsubscribe(topic: string, callback: (error?: Error) => void): this {
    this.unsubscriptions.push(topic);
    callback();
    return this;
  }

  publish(_topic: string, _payload: string, _options: unknown, callback: () => void): this {
    callback();
    return this;
  }

  end(_force: boolean, _options: unknown, callback: () => void): this {
    callback();
    return this;
  }
}

const logger: Logger = {
  debug: () => undefined,
  info: () => undefined,
  warn: () => undefined,
  error: () => undefined
};

describe("Route Service MQTT 网关", () => {
  beforeEach(() => {
    mqtt.client = null;
  });

  it("启动只常驻订阅低频事件和命令ACK并按兴趣精确订阅实时Topic", async () => {
    const gateway = createGateway();
    await gateway.start();
    const client = mqtt.client!;

    expect(client.subscriptions).toEqual(expect.arrayContaining([
      "cns_rpi/events/devices/+/state",
      "cns_rpi/sources/web/config/ack",
      "cns_rpi/sources/web/control/ack"
    ]));
    expect(client.subscriptions).not.toContain("cns_rpi/+/px4/realtime/v1");
    expect(client.subscriptions).not.toContain("cns_rpi/+/telemetry/realtime/v1");
    expect(client.subscriptions).not.toContain("cns_rpi/+/px4/latency/ack/v1");

    await gateway.setRealtimeInterest(deviceId, true);
    expect(client.subscriptions).toContain(
      `cns_rpi/${deviceId}/telemetry/realtime/v1`
    );
    await gateway.setRealtimeInterest(deviceId, false);
    expect(client.unsubscriptions).toContain(
      `cns_rpi/${deviceId}/telemetry/realtime/v1`
    );
    await gateway.close();
  });

  it("只转交Topic与payload设备一致的v1通用实时帧", async () => {
    const received: unknown[] = [];
    const gateway = createGateway((frame) => received.push(frame));
    await gateway.start();
    const frame = {
      schema_version: 1,
      device_id: deviceId,
      sequence: 7,
      sent_at: "2026-08-05T01:02:03.123Z",
      telemetry: { attitude: { roll: 1.2 } }
    };

    mqtt.client!.emit(
      "message",
      `cns_rpi/${deviceId}/telemetry/realtime/v1`,
      Buffer.from(JSON.stringify(frame))
    );
    mqtt.client!.emit(
      "message",
      `cns_rpi/${deviceId}/px4/realtime/v1`,
      Buffer.from(JSON.stringify(frame))
    );
    mqtt.client!.emit(
      "message",
      `cns_rpi/${deviceId}/telemetry/realtime/v1`,
      Buffer.from(JSON.stringify({ ...frame, schema_version: 3 }))
    );
    mqtt.client!.emit(
      "message",
      `cns_rpi/CNS00000000000000002/telemetry/realtime/v1`,
      Buffer.from(JSON.stringify(frame))
    );

    expect(received).toEqual([frame]);
    await gateway.close();
  });
});

function createGateway(onDeviceRealtime: (frame: unknown) => void = () => undefined) {
  const config = AppConfigSchema.parse({
    mqtt: {
      source_id: "web",
      connect_timeout_ms: 1000
    }
  });
  return createRouteServiceGateway(config.mqtt, {
    onDeviceState: () => undefined,
    onCommandAck: () => undefined,
    onDeviceRealtime
  }, logger);
}
