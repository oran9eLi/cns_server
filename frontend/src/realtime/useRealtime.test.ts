import { describe, expect, it } from "vitest";
import type {
  DeviceDetailResponse,
  DeviceListResponse,
  DeviceStateEvent
} from "@cns/backend-protocol";

import { mergeDeviceDetailState, mergeDeviceListState } from "./useRealtime.js";

const event: DeviceStateEvent = {
  type: "device.state",
  schema_version: 1,
  device_id: "PX4RID123456789ABCDE",
  device_type: "flight_controller",
  school_name: null,
  dcdw_label: null,
  capabilities: ["telemetry", "remote_id"],
  product: { model_code: "7" },
  version: { firmware: "1.17.3" },
  status: "online",
  event_at: "2026-08-03T10:00:01.000Z",
  last_seen_at: "2026-08-03T10:00:01.000Z",
  telemetry_received_at: "2026-08-03T10:00:01.000Z",
  latest_telemetry: { schema_version: 3 },
  change_reason: "registration_online",
  degraded: false
};

const oldItem = {
  device_id: event.device_id,
  device_type: event.device_type,
  school_name: null,
  dcdw_label: null,
  model_version: "PX4",
  capabilities: null,
  product: null,
  version: null,
  status: "offline" as const,
  last_seen_at: null,
  telemetry_received_at: null,
  degraded: false
};

describe("device.state cache merge", () => {
  it("updates registration metadata in both list and detail caches", () => {
    const list: DeviceListResponse = { schema_version: 1, items: [oldItem] };
    const detail: DeviceDetailResponse = {
      schema_version: 1,
      item: { ...oldItem, provisioned_at: null, latest_telemetry: null }
    };

    expect(mergeDeviceListState(list, event).items[0]).toMatchObject({
      capabilities: event.capabilities,
      product: event.product,
      version: event.version
    });
    expect(mergeDeviceDetailState(detail, event).item).toMatchObject({
      capabilities: event.capabilities,
      product: event.product,
      version: event.version
    });
  });
});
