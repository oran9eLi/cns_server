import type { DeviceDetail } from "@cns/backend-protocol";

export function createSeedDevices(): DeviceDetail[] {
  const now = new Date().toISOString();

  return [
    createDevice("CNS00000000000000001", "DCDW-001", "东创航空实训中心", "CNS v1.0", "online", now),
    createDevice("CNS00000000000000002", "DCDW-002", "东创航空实训中心", "CNS v1.1", "online", now),
    createDevice("CNS00000000000000003", "DCDW-003", "东创航空实训中心", "CNS v1.0", "offline", "2026-07-20T13:30:00.000+08:00"),
    createDevice("CNS00000000000000004", null, "东创航空实训中心", "CNS v1.1", "online", now, true),
    createDevice("CNS00000000000000005", "DCDW-101", "华东无人系统学院", "CNS v1.0", "online", now),
    createDevice("CNS00000000000000006", "DCDW-102", "华东无人系统学院", "CNS v1.1", "offline", "2026-07-20T12:45:00.000+08:00"),
    createDevice("CNS00000000000000007", "DCDW-103", "华东无人系统学院", "CNS v1.0", "online", now),
    createDevice("CNS00000000000000008", null, "华东无人系统学院", "CNS v1.1", "online", "2026-07-20T12:15:00.000+08:00", true),
    createPx4Device(now)
  ];
}

function createPx4Device(now: string): DeviceDetail {
  const deviceId = "PX4U2-00112233445566778899AABBCCDDEEFF0011";
  return {
    device_id: deviceId,
    device_type: "flight_controller",
    vendor_id: deviceId,
    school_name: null,
    dcdw_label: null,
    model_version: "PX4",
    status: "online",
    last_seen_at: now,
    telemetry_received_at: now,
    degraded: false,
    provisioned_at: now,
    latest_telemetry: {
      schema_version: 2,
      device_id: deviceId,
      device_type: "flight_controller",
      identity: {
        uid2: "00112233445566778899AABBCCDDEEFF0011",
        remote_id: "1581F3411C32233939383438"
      }
    }
  };
}

function createDevice(
  vendorId: string,
  dcdwLabel: string | null,
  schoolName: string,
  modelVersion: string,
  status: "online" | "offline",
  lastSeenAt: string,
  degraded = false
): DeviceDetail {
  const seed = Number(vendorId.slice(-2));
  const online = status === "online";

  return {
    device_id: vendorId,
    device_type: "cns_box",
    vendor_id: vendorId,
    school_name: schoolName,
    dcdw_label: dcdwLabel,
    model_version: modelVersion,
    status,
    last_seen_at: lastSeenAt,
    telemetry_received_at: online ? lastSeenAt : null,
    degraded,
    provisioned_at: `2026-07-${String(10 + seed).padStart(2, "0")}T14:30:00.000+08:00`,
    latest_telemetry: online
      ? {
          attitude: {
            roll_deg: -5 + seed * 0.7,
            pitch_deg: -2 + seed * 0.25,
            yaw_deg: (280 + seed * 6) % 360
          },
          environment: {
            temperature_c: 25 + seed * 0.3,
            pressure_hpa: 1010 - seed * 0.2,
            altitude_m: 12 + seed * 0.9
          },
          link: {
            rssi_dbm: -48 - seed,
            packet_loss_pct: degraded ? 8.5 : 0.2,
            latency_ms: 20 + seed * 2
          },
          motors: {
            pwm: [990 + seed, 986 + seed, 982 + seed, 978 + seed]
          }
        }
      : null
  };
}
