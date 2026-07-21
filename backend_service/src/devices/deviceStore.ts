import type {
  DeviceDetail,
  DeviceListQuery,
  DeviceSummary,
  DependencyStatus
} from "@cns/backend-protocol";

export interface DeviceStore {
  list(query: DeviceListQuery): Promise<DeviceSummary[]>;
  get(vendorId: string): Promise<DeviceDetail | null>;
  dependencyStatus(): Promise<DependencyStatus>;
  close(): Promise<void>;
}

export function createInMemoryDeviceStore(initialDevices: DeviceDetail[]): DeviceStore {
  const devices = new Map(initialDevices.map((device) => [device.vendor_id, structuredClone(device)]));

  return {
    async list(query) {
      return Array.from(devices.values())
        .filter((device) => matchesQuery(device, query))
        .map(toSummary);
    },
    async get(vendorId) {
      const device = devices.get(vendorId);
      return device ? structuredClone(device) : null;
    },
    async dependencyStatus() {
      return "not_configured";
    },
    async close() {
      return undefined;
    }
  };
}

function matchesQuery(device: DeviceDetail, query: DeviceListQuery): boolean {
  if (query.status && device.status !== query.status) return false;
  if (query.school_name && device.school_name !== query.school_name) return false;

  if (!query.keyword) return true;
  const keyword = query.keyword.toLowerCase();
  return [
    device.vendor_id,
    device.school_name,
    device.dcdw_label ?? "",
    device.model_version
  ].some((value) => value.toLowerCase().includes(keyword));
}

function toSummary(device: DeviceDetail): DeviceSummary {
  return {
    vendor_id: device.vendor_id,
    school_name: device.school_name,
    dcdw_label: device.dcdw_label,
    model_version: device.model_version,
    status: device.status,
    last_seen_at: device.last_seen_at,
    telemetry_received_at: device.telemetry_received_at,
    degraded: device.degraded
  };
}
