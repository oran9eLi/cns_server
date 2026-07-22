import type { JsonValue } from "@cns/backend-protocol";

export interface FlightPowerChannel {
  voltage: number | null;
  remaining: number | null;
}

export interface FlightModuleState {
  key: string;
  label: string;
  status: "normal" | "warning" | "error" | "data" | "unknown";
  detail: string;
}

export interface FlightAlertItem {
  id: string;
  code: string | null;
  level: string;
  message: string;
  occurredAt: string | null;
}

export interface FlightLogItem {
  id: string;
  level: string;
  message: string;
  occurredAt: string | null;
}

export interface FlightPositionView {
  fixValid: boolean | null;
  fixType: number | null;
  latitudeWgs84: number | null;
  longitudeWgs84: number | null;
  latitudeGcj02: number | null;
  longitudeGcj02: number | null;
  heading: number | null;
  altitude: number | null;
  satellites: number | null;
  horizontalAccuracy: number | null;
}

export interface FlightDashboardView {
  position: FlightPositionView;
  environment: {
    altitude: number | null;
    humidity: number | null;
  };
  power: {
    controller: FlightPowerChannel;
    motor: FlightPowerChannel;
  };
  modules: FlightModuleState[];
  alerts: FlightAlertItem[];
  logs: FlightLogItem[];
}

type JsonRecord = { [key: string]: JsonValue };

const MODULE_DEFINITIONS = [
  { key: "position", label: "定位模块", aliases: ["position", "gps", "location"], moduleNames: ["GNSS"] },
  { key: "attitude", label: "姿态模块", aliases: ["attitude", "imu"], moduleNames: ["IMU"] },
  { key: "environment", label: "环境模块", aliases: ["environment", "pressure", "sensor"], moduleNames: ["BARO"] },
  { key: "lora", label: "LoRa 通信", aliases: ["lora"], moduleNames: ["LORA"] },
  { key: "cellular", label: "5G 通信", aliases: ["cellular_5g", "5g", "cellular"], moduleNames: ["5G"] },
  { key: "remote_id", label: "RemoteID", aliases: ["remote_id", "remoteid"], moduleNames: ["REMOTE_ID"] },
  { key: "storage", label: "存储模块", aliases: ["storage", "disk", "sdcard"], moduleNames: ["STORAGE"] },
  { key: "motor", label: "电机模块", aliases: ["motor", "motors"], moduleNames: ["MOTOR", "CONTROL"] }
] as const;

export function mapFlightDashboard(source: JsonValue | null | undefined): FlightDashboardView {
  return {
    position: mapFlightPosition(source),
    environment: {
      altitude: firstNumber(source, [
        "telemetry.pressure.altitude",
        "telemetry.pressure.altitude_m",
        "telemetry.environment.altitude",
        "telemetry.environment.altitude_m",
        "telemetry.position.altitude",
        "telemetry.position.altitude_m",
        "telemetry.position.relative_altitude",
        "telemetry.gps.altitude",
        "telemetry.gps.alt",
        "telemetry.global_position.alt",
        "environment.altitude_m"
      ]),
      humidity: firstNumber(source, [
        "telemetry.pressure.humidity",
        "telemetry.pressure.humidity_percent",
        "telemetry.environment.humidity",
        "telemetry.environment.humidity_percent",
        "telemetry.humidity.humidity_percent",
        "environment.humidity",
        "environment.humidity_percent"
      ])
    },
    power: {
      controller: {
        voltage: firstNumber(source, [
          "telemetry.sys_status.voltage_battery",
          "telemetry.battery.voltage_battery",
          "telemetry.power.controller.voltage_mv",
          "telemetry.power.controller.voltage",
          "telemetry.battery.controller.voltage_mv",
          "telemetry.battery.controller.voltage",
          "telemetry.battery.main_control.voltage",
          "telemetry.battery.main.voltage",
          "telemetry.battery.battery_1.voltage",
          "telemetry.battery.battery1.voltage",
          "telemetry.batteries[0].voltage"
        ]) ?? batteryPackVoltage(source, "telemetry.battery.voltages") ?? firstNumber(source, [
          "battery.voltages[0]",
          "batteries[0].voltage"
        ]),
          remaining: firstNumber(source, [
          "telemetry.battery.battery_remaining",
          "telemetry.sys_status.battery_remaining",
          "telemetry.power.controller.remaining_percent",
          "telemetry.power.controller.remaining",
          "telemetry.battery.controller.remaining",
          "telemetry.battery.main_control.remaining",
          "telemetry.battery.main.remaining",
          "telemetry.battery.battery_1.remaining",
          "telemetry.battery.battery1.remaining",
          "telemetry.batteries[0].remaining",
          "telemetry.battery.battery_remaining[0]",
          "battery.battery_remaining[0]",
          "batteries[0].remaining"
        ])
      },
      motor: {
        voltage: firstNumber(source, [
          "telemetry.battery2.voltage_battery",
          "telemetry.power.motor.voltage_mv",
          "telemetry.power.motor.voltage",
          "telemetry.battery.motor.voltage_mv",
          "telemetry.battery.motor.voltage",
          "telemetry.battery.battery_2.voltage",
          "telemetry.battery.battery2.voltage",
          "telemetry.batteries[1].voltage"
        ]) ?? batteryPackVoltage(source, "telemetry.battery2.voltages") ?? firstNumber(source, [
          "telemetry.battery.voltages[1]",
          "battery.voltages[1]",
          "battery.voltage_battery",
          "batteries[1].voltage"
        ]),
        remaining: firstNumber(source, [
          "telemetry.battery2.battery_remaining",
          "telemetry.power.motor.remaining_percent",
          "telemetry.power.motor.remaining",
          "telemetry.battery.motor.remaining",
          "telemetry.battery.battery_2.remaining",
          "telemetry.battery.battery2.remaining",
            "telemetry.batteries[1].remaining",
            "telemetry.battery.battery_remaining[1]",
            "battery.battery_remaining[1]",
            "batteries[1].remaining"
          ])
      }
    },
    modules: MODULE_DEFINITIONS.map((definition) => mapModule(source, definition)),
    alerts: normalizeAlerts(firstValue(source, [
      "alerts",
      "alarms",
      "warnings",
      "telemetry.alerts",
      "telemetry.alarms",
      "telemetry.warnings",
      "telemetry.alarm"
    ])),
    logs: normalizeLogs(firstValue(source, [
      "logs",
      "messages",
      "events",
      "telemetry.logs",
      "telemetry.messages",
      "telemetry.events",
      "telemetry.log"
    ]))
  };
}

function mapFlightPosition(source: JsonValue | null | undefined): FlightPositionView {
  return {
    fixValid: positionFixValidity(source),
    fixType: firstNumber(source, [
      "telemetry.gps.fix_type",
      "telemetry.position.fix_type",
      "gps.fix_type",
      "position.fix_type"
    ]),
    latitudeWgs84: normalizeCoordinate(firstNumber(source, [
      "telemetry.position.latitude_wgs84",
      "telemetry.position.latitude_deg",
      "telemetry.position.latitude",
      "telemetry.position.lat_deg",
      "telemetry.position.lat",
      "telemetry.gps.latitude_wgs84",
      "telemetry.gps.latitude",
      "telemetry.gps.lat",
      "telemetry.global_position.latitude",
      "telemetry.global_position.lat",
      "position.latitude_wgs84",
      "position.latitude",
      "position.lat",
      "gps.latitude_wgs84",
      "gps.latitude",
      "gps.lat",
      "global_position.latitude",
      "global_position.lat"
    ]), 90),
    longitudeWgs84: normalizeCoordinate(firstNumber(source, [
      "telemetry.position.longitude_wgs84",
      "telemetry.position.longitude_deg",
      "telemetry.position.longitude",
      "telemetry.position.lon_deg",
      "telemetry.position.lng_deg",
      "telemetry.position.lon",
      "telemetry.position.lng",
      "telemetry.gps.longitude_wgs84",
      "telemetry.gps.longitude",
      "telemetry.gps.lon",
      "telemetry.gps.lng",
      "telemetry.global_position.longitude",
      "telemetry.global_position.lon",
      "telemetry.global_position.lng",
      "position.longitude_wgs84",
      "position.longitude",
      "position.lon",
      "position.lng",
      "gps.longitude_wgs84",
      "gps.longitude",
      "gps.lon",
      "gps.lng",
      "global_position.longitude",
      "global_position.lon",
      "global_position.lng"
    ]), 180),
    latitudeGcj02: normalizeCoordinate(firstNumber(source, [
      "telemetry.position.latitude_gcj02",
      "telemetry.gps.latitude_gcj02",
      "position.latitude_gcj02",
      "gps.latitude_gcj02"
    ]), 90),
    longitudeGcj02: normalizeCoordinate(firstNumber(source, [
      "telemetry.position.longitude_gcj02",
      "telemetry.gps.longitude_gcj02",
      "position.longitude_gcj02",
      "gps.longitude_gcj02"
    ]), 180),
    heading: firstNumber(source, [
      "telemetry.position.heading_deg",
      "telemetry.position.heading",
      "telemetry.position.course_deg",
      "telemetry.gps.heading_deg",
      "telemetry.gps.heading",
      "telemetry.global_position.heading_deg",
      "telemetry.global_position.heading",
      "telemetry.global_position.hdg",
      "position.heading_deg",
      "position.heading",
      "gps.heading_deg",
      "gps.heading",
      "global_position.heading_deg",
      "global_position.heading",
      "global_position.hdg"
    ]),
    altitude: firstNumber(source, [
      "telemetry.gps.altitude",
      "telemetry.gps.alt",
      "telemetry.global_position.altitude",
      "telemetry.global_position.alt",
      "telemetry.position.altitude",
      "telemetry.position.alt",
      "gps.altitude",
      "gps.alt",
      "global_position.altitude",
      "global_position.alt",
      "position.altitude",
      "position.alt"
    ]),
    satellites: firstNumber(source, [
      "telemetry.gps.satellites_visible",
      "telemetry.gps.satellites",
      "telemetry.gps.satellite_count",
      "gps.satellites_visible",
      "gps.satellites",
      "gps.satellite_count"
    ]),
    horizontalAccuracy: firstNumber(source, [
      "telemetry.gps.h_acc",
      "telemetry.gps.horizontal_accuracy",
      "telemetry.gps.horizontal_accuracy_m",
      "gps.h_acc",
      "gps.horizontal_accuracy",
      "gps.horizontal_accuracy_m"
    ])
  };
}

function positionFixValidity(source: JsonValue | null | undefined): boolean | null {
  const explicit = firstValue(source, [
    "telemetry.position.fix_valid",
    "telemetry.position.valid",
    "telemetry.position.has_fix",
    "telemetry.gps.fix_valid",
    "telemetry.gps.valid",
    "telemetry.gps.has_fix",
    "position.fix_valid",
    "position.valid",
    "gps.fix_valid"
  ]);
  if (typeof explicit === "boolean") return explicit;
  if (explicit === 0 || explicit === "0" || explicit === "false") return false;
  if (explicit === 1 || explicit === "1" || explicit === "true") return true;

  const fixType = firstValue(source, [
    "telemetry.position.fix_type",
    "telemetry.position.fix",
    "telemetry.gps.fix_type",
    "telemetry.gps.fix",
    "position.fix_type",
    "gps.fix_type"
  ]);
  if (typeof fixType === "number" && Number.isFinite(fixType)) return fixType >= 2;
  if (typeof fixType === "string" && fixType.trim() !== "") {
    const numericFixType = Number(fixType);
    if (Number.isFinite(numericFixType)) return numericFixType >= 2;
  }
  if (typeof fixType !== "string" || fixType.trim() === "") return null;
  const normalized = fixType.trim().toLocaleLowerCase();
  return !["none", "no_fix", "nofix", "invalid", "offline"].includes(normalized);
}

function normalizeCoordinate(value: number | null, maximum: number): number | null {
  if (value === null) return null;
  const normalized = Math.abs(value) > maximum && Math.abs(value) <= maximum * 10_000_000
    ? value / 10_000_000
    : value;
  return Math.abs(normalized) <= maximum ? normalized : null;
}

export function readFlightValue(
  source: JsonValue | null | undefined,
  path: string
): JsonValue | undefined {
  const parts = path.replace(/\[(\d+)\]/g, ".$1").split(".").filter(Boolean);
  let current: JsonValue | undefined = source ?? undefined;

  for (const part of parts) {
    if (Array.isArray(current)) {
      current = current[Number(part)];
    } else if (isRecord(current)) {
      current = current[part];
    } else {
      return undefined;
    }
  }

  return current;
}

function mapModule(
  source: JsonValue | null | undefined,
  definition: typeof MODULE_DEFINITIONS[number]
): FlightModuleState {
  const reportedModule = findReportedModule(source, definition.moduleNames);
  if (reportedModule !== undefined) {
    const parsed = parseModuleStatus(reportedModule);
    if (parsed) return { key: definition.key, label: definition.label, ...parsed };
  }

  const statusPaths = definition.aliases.flatMap((alias) => [
    `self_check.${alias}`,
    `self_check.${alias}.status`,
    `modules.${alias}`,
    `modules.${alias}.status`,
    `telemetry.self_check.${alias}`,
    `telemetry.self_check.${alias}.status`,
    `telemetry.modules.${alias}`,
    `telemetry.modules.${alias}.status`,
    `telemetry.health.${alias}`,
    `telemetry.health.${alias}.status`
  ]);
  const explicit = firstValue(source, statusPaths);
  const parsed = parseModuleStatus(explicit);
  if (parsed) return { key: definition.key, label: definition.label, ...parsed };

  const evidence = firstValue(source, definition.aliases.flatMap((alias) => [
    `telemetry.${alias}`,
    alias
  ]));

  if (evidence !== undefined && evidence !== null) {
    return {
      key: definition.key,
      label: definition.label,
      status: "data",
      detail: "已上报"
    };
  }

  return {
    key: definition.key,
    label: definition.label,
    status: "unknown",
    detail: "暂无数据"
  };
}

function parseModuleStatus(value: JsonValue | undefined): Pick<FlightModuleState, "status" | "detail"> | null {
  if (value === undefined || value === null) return null;

  if (isRecord(value)) {
    for (const key of ["status", "state", "healthy", "online", "ok", "passed", "available"]) {
      if (value[key] !== undefined) return parseModuleStatus(value[key]);
    }
    return null;
  }

  if (typeof value === "boolean") {
    return { status: value ? "normal" : "error", detail: value ? "正常" : "异常" };
  }

  const normalized = String(value).trim().toLocaleLowerCase();
  if (["1", "ok", "normal", "online", "ready", "healthy", "pass", "passed", "available"].includes(normalized)) {
    return { status: "normal", detail: "正常" };
  }
  if (["warning", "warn", "degraded", "unstable", "starting"].includes(normalized)) {
    return { status: "warning", detail: normalized === "starting" ? "启动中" : "告警" };
  }
  if (["0", "error", "failed", "fail", "offline", "fault", "unavailable", "disconnected"].includes(normalized)) {
    return { status: "error", detail: "异常" };
  }
  if (normalized === "uninitialized") return { status: "unknown", detail: "未初始化" };
  if (normalized === "disabled") return { status: "unknown", detail: "已禁用" };
  return { status: "data", detail: String(value) };
}

function normalizeAlerts(value: JsonValue | undefined): FlightAlertItem[] {
  return collectionItems(unwrapEntries(value)).map(([key, item], index) => {
    const record = isRecord(item) ? item : null;
    const code = stringField(record, ["code", "fault_code", "alarm_code", "warning_code", "id"])
      ?? (key === String(index) ? null : key);
    const message = stringField(record, ["message", "msg", "description", "content", "detail"])
      ?? primitiveText(item)
      ?? (code ? `故障码 ${code}` : "未提供告警内容");
    const level = normalizeLevel(stringField(record, ["level", "severity", "status", "type"]), "warning");
    const occurredAt = stringField(record, ["occurred_at", "time", "timestamp", "created_at"]);
    return { id: `alert-${key}-${index}`, code, level, message, occurredAt };
  });
}

function normalizeLogs(value: JsonValue | undefined): FlightLogItem[] {
  return collectionItems(unwrapEntries(value)).map(([key, item], index) => {
    const record = isRecord(item) ? item : null;
    const messageId = stringField(record, ["message_id"]);
    const message = stringField(record, ["message", "msg", "description", "content", "detail", "event"])
      ?? messageLogText(messageId)
      ?? primitiveText(item)
      ?? `${key}: ${JSON.stringify(item)}`;
    const level = normalizeLevel(stringField(record, ["level", "severity", "status", "type"]), "info");
    const occurredAt = stringField(record, ["occurred_at", "time", "timestamp", "created_at", "event_at"]);
    const sequence = stringField(record, ["sequence"]);
    return { id: `log-${sequence ?? key}-${index}`, level, message, occurredAt };
  });
}

function unwrapEntries(value: JsonValue | undefined): JsonValue | undefined {
  if (!isRecord(value)) return value;
  return Array.isArray(value.entries) ? value.entries : value;
}

const MESSAGE_LOG_TEXT: Record<number, string> = {
  0: "系统启动",
  1: "自检通过",
  2: "自检部分通过",
  3: "自检未通过",
  4: "定位正常",
  5: "定位无信号",
  6: "定位断开",
  7: "姿态正常",
  8: "姿态断开",
  9: "环境正常",
  10: "环境断开",
  11: "LoRa 正常",
  12: "LoRa 断开",
  13: "存储正常",
  14: "存储断开",
  15: "电机正常",
  16: "电机断开",
  17: "电机供电不足",
  18: "一号电机故障",
  19: "二号电机故障",
  20: "三号电机故障",
  21: "四号电机故障",
  22: "全部电机故障",
  23: "电机电池没电",
  24: "电机电池需充电",
  25: "主控电池需充电",
  26: "有告警",
  27: "无告警",
  28: "RemoteID 正常",
  29: "RemoteID 断开",
  30: "5G 正常",
  31: "5G 断开",
  32: "姿态或环境异常，一键起飞失败",
  33: "电机供电不足，一键起飞失败",
  34: "电机供电不足，油门失败",
  35: "一键降落失败",
  36: "电机供电不足，自动停机",
  37: "降落中，油门失败"
};

function messageLogText(messageId: string | null): string | null {
  if (messageId === null) return null;
  const id = Number(messageId);
  return Number.isInteger(id) ? MESSAGE_LOG_TEXT[id] ?? `消息 ID ${id}` : null;
}

function normalizeLevel(value: string | null, fallback: string): string {
  if (value === null) return fallback;
  const numeric = Number(value);
  if (Number.isInteger(numeric)) {
    return (["info", "warning", "error", "critical"] as const)[numeric] ?? fallback;
  }
  return value.toLocaleLowerCase();
}

function findReportedModule(
  source: JsonValue | null | undefined,
  names: readonly string[]
): JsonValue | undefined {
  const expected = new Set(names.map((name) => name.toLocaleUpperCase()));
  for (const path of ["modules", "telemetry.modules"]) {
    const modules = readFlightValue(source, path);
    if (!Array.isArray(modules)) continue;
    for (const module of modules) {
      if (!isRecord(module) || typeof module.name !== "string") continue;
      if (!expected.has(module.name.toLocaleUpperCase())) continue;
      return module.status ?? module.state;
    }
  }
  return undefined;
}

function batteryPackVoltage(
  source: JsonValue | null | undefined,
  path: string
): number | null {
  const values = readFlightValue(source, path);
  if (!Array.isArray(values)) return null;
  const cells = values.filter(
    (value): value is number => typeof value === "number" && Number.isFinite(value) && value > 0
  );
  if (cells.some((value) => value >= 100)) return cells[0] ?? null;
  return cells.length > 0 ? cells.reduce((sum, value) => sum + value, 0) : null;
}

function collectionItems(value: JsonValue | undefined): Array<[string, JsonValue]> {
  if (value === undefined || value === null) return [];
  if (Array.isArray(value)) return value.map((item, index) => [String(index), item]);
  if (isRecord(value)) {
    const looksLikeSingleItem = ["message", "msg", "description", "content", "code"].some(
      (key) => value[key] !== undefined
    );
    return looksLikeSingleItem ? [["0", value]] : Object.entries(value);
  }
  return [["0", value]];
}

function firstNumber(source: JsonValue | null | undefined, paths: string[]): number | null {
  for (const path of paths) {
    const value = readFlightValue(source, path);
    if (typeof value === "number" && Number.isFinite(value)) return value;
    if (typeof value === "string" && value.trim() !== "") {
      const parsed = Number(value);
      if (Number.isFinite(parsed)) return parsed;
    }
  }
  return null;
}

function firstValue(source: JsonValue | null | undefined, paths: readonly string[]): JsonValue | undefined {
  for (const path of paths) {
    const value = readFlightValue(source, path);
    if (value !== undefined) return value;
  }
  return undefined;
}

function stringField(record: JsonRecord | null, keys: string[]): string | null {
  if (!record) return null;
  for (const key of keys) {
    const value = record[key];
    if (["string", "number", "boolean"].includes(typeof value)) return String(value);
  }
  return null;
}

function primitiveText(value: JsonValue): string | null {
  return ["string", "number", "boolean"].includes(typeof value) ? String(value) : null;
}

function isRecord(value: JsonValue | undefined): value is JsonRecord {
  return Boolean(value) && typeof value === "object" && !Array.isArray(value);
}
