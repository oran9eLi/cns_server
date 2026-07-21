import type {
  DeviceDetail,
  DeviceListQuery,
  DeviceSummary,
  DependencyStatus,
  JsonValue
} from "@cns/backend-protocol";

export interface DeviceStore {
  list(query: DeviceListQuery): Promise<DeviceSummary[]>;
  get(vendorId: string): Promise<DeviceDetail | null>;
  updateTelemetry(vendorId: string, telemetry: Record<string, JsonValue>): Promise<DeviceDetail | null>;
  setMotorPwm(vendorId: string, motorPwm: [number, number, number, number]): Promise<DeviceDetail | null>;
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
    async updateTelemetry(vendorId, telemetry) {
      const device = devices.get(vendorId);
      if (!device) return null;

      const now = new Date().toISOString();
      device.latest_telemetry = structuredClone(telemetry);
      device.telemetry_received_at = now;
      device.last_seen_at = now;
      device.status = "online";
      device.degraded = false;
      return structuredClone(device);
    },
    async setMotorPwm(vendorId, motorPwm) {
      const device = devices.get(vendorId);
      if (!device) return null;

      device.latest_telemetry = {
        ...(device.latest_telemetry ?? {}),
        motors: {
          ...readObject(device.latest_telemetry?.motors),
          pwm: motorPwm
        }
      };
      device.telemetry_received_at = new Date().toISOString();
      return structuredClone(device);
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

function readObject(value: JsonValue | undefined): Record<string, JsonValue> {
  if (!value || Array.isArray(value) || typeof value !== "object") {
    return {};
  }
  return value;
}
