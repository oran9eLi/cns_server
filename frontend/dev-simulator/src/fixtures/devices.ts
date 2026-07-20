import type { DeviceDetail, JsonValue } from "@cns/backend-protocol";

const now = new Date("2026-07-20T06:30:00.000Z").toISOString();

export const initialDevices: DeviceDetail[] = [
  device("CNS00000000000000001", "东创航空实训中心", "DCDW-001", "online", false, 0),
  device("CNS00000000000000002", "东创航空实训中心", "DCDW-002", "online", false, 1),
  device("CNS00000000000000003", "东创航空实训中心", "DCDW-003", "offline", false, 2),
  device("CNS00000000000000004", "东创航空实训中心", null, "online", true, 3),
  device("CNS00000000000000005", "华东无人系统学院", "DCDW-101", "online", false, 4),
  device("CNS00000000000000006", "华东无人系统学院", "DCDW-102", "offline", false, 5),
  device("CNS00000000000000007", "华东无人系统学院", "DCDW-103", "online", false, 6),
  device("CNS00000000000000008", "华东无人系统学院", null, "offline", true, 7)
];

function device(
  vendor_id: string,
  school_name: string,
  dcdw_label: string | null,
  status: "online" | "offline",
  degraded: boolean,
  index: number
): DeviceDetail {
  const online = status === "online";
  return {
    vendor_id,
    school_name,
    dcdw_label,
    model_version: index % 2 === 0 ? "CNS v1.0" : "CNS v1.1",
    status,
    last_seen_at: online ? now : new Date(Date.parse(now) - (index + 2) * 900000).toISOString(),
    telemetry_received_at: online ? now : null,
    degraded,
    provisioned_at: new Date(Date.parse(now) - (index + 8) * 86400000).toISOString(),
    latest_telemetry: online
      ? {
          identity: {
            vendor_id,
            dcdw_label,
            school_name
          },
          attitude: {
            roll_deg: round((index - 3) * 1.7),
            pitch_deg: round((index % 3) * 2.1 - 1.6),
            yaw_deg: round(42 + index * 17.4)
          },
          environment: {
            temperature_c: round(25.4 + index * 0.7),
            pressure_hpa: round(1009.6 - index * 0.8),
            altitude_m: round(12.5 + index * 1.9)
          },
          link: {
            rssi_dbm: -48 - index * 3,
            packet_loss_pct: round(index * 0.8),
            latency_ms: 18 + index * 4
          },
          motors: {
            pwm: [980 + index * 7, 982 + index * 5, 979 + index * 6, 981 + index * 4]
          },
          runtime_config: {
            telemetry_publish_interval_ms: 2000
          }
        }
      : index === 5
        ? {
            identity: { vendor_id, dcdw_label, school_name },
            link: { rssi_dbm: -86 }
          }
        : null
  };
}

export function cloneInitialDevices(): DeviceDetail[] {
  return structuredClone(initialDevices);
}

export function nudgeTelemetry(device: DeviceDetail, tick: number): DeviceDetail {
  if (device.status !== "online" || !device.latest_telemetry) {
    return device;
  }

  const telemetry = structuredClone(device.latest_telemetry) as Record<string, JsonValue>;
  const attitude = telemetry.attitude as Record<string, number> | undefined;
  const environment = telemetry.environment as Record<string, number> | undefined;
  const link = telemetry.link as Record<string, number> | undefined;
  const motors = telemetry.motors as { pwm?: number[] } | undefined;

  if (attitude) {
    attitude.roll_deg = round((attitude.roll_deg ?? 0) + Math.sin(tick / 3) * 0.3);
    attitude.pitch_deg = round((attitude.pitch_deg ?? 0) + Math.cos(tick / 4) * 0.2);
    attitude.yaw_deg = round(((attitude.yaw_deg ?? 0) + 0.8) % 360);
  }
  if (environment) {
    environment.temperature_c = round((environment.temperature_c ?? 25) + Math.sin(tick / 5) * 0.08);
    environment.altitude_m = round((environment.altitude_m ?? 10) + Math.cos(tick / 6) * 0.12);
  }
  if (link) {
    link.latency_ms = Math.max(12, Math.round((link.latency_ms ?? 20) + Math.sin(tick / 2) * 2));
  }
  if (motors?.pwm) {
    motors.pwm = motors.pwm.map((value, motorIndex) => Math.round(value + Math.sin((tick + motorIndex) / 2) * 4));
  }

  const receivedAt = new Date().toISOString();
  return {
    ...device,
    last_seen_at: receivedAt,
    telemetry_received_at: receivedAt,
    latest_telemetry: telemetry
  };
}

function round(value: number): number {
  return Math.round(value * 100) / 100;
}
