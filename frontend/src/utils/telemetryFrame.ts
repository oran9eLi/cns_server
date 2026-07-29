import type { JsonValue } from "@cns/backend-protocol";

export type TelemetryValueType = "number" | "string" | "boolean" | "null" | "empty";

export interface TelemetryFrameEntry {
  path: string;
  label: string;
  value: JsonValue;
  displayValue: string;
  unit: string | null;
  type: TelemetryValueType;
}

export interface TelemetryFrameGroup {
  id: string;
  path: string;
  title: string;
  entries: TelemetryFrameEntry[];
}

export interface TelemetryFrameSummary {
  fieldCount: number;
  populatedCount: number;
  groupCount: number;
  byteCount: number;
}

const LABELS: Record<string, string> = {
  telemetry: "遥测",
  identity: "设备身份",
  attitude: "飞行姿态",
  pressure: "环境与气压",
  environment: "环境",
  motor: "电机",
  motors: "电机",
  lora: "LoRa 链路",
  link: "通信链路",
  battery: "电池",
  battery2: "电池 2",
  gps: "定位",
  global_position: "全局位置",
  cellular_5g: "5G 链路",
  humidity: "湿度",
  gnss_sat: "卫星统计",
  gnss_time: "卫星时间",
  modules: "模块自检",
  alarms: "告警",
  logs: "消息日志",
  drone_id: "RemoteID",
  position: "定位",
  system: "系统",
  status: "状态",
  roll: "横滚 Roll",
  roll_deg: "横滚 Roll",
  pitch: "俯仰 Pitch",
  pitch_deg: "俯仰 Pitch",
  yaw: "偏航 Yaw",
  yaw_deg: "偏航 Yaw",
  temperature: "温度",
  temperature_c: "温度",
  press_abs: "绝对气压",
  pressure_hpa: "气压",
  altitude_m: "高度",
  alt: "高度",
  alt_ellipsoid: "椭球高度",
  hdg: "航向",
  fix_type: "定位类型",
  satellites_visible: "可见卫星",
  h_acc: "水平精度",
  v_acc: "垂直精度",
  pwm_us: "电机 PWM",
  loss_rate_percent: "丢包率",
  packet_loss_pct: "丢包率",
  packet_loss_percent: "丢包率",
  rssi_dbm: "信号强度 RSSI",
  latency_ms: "链路延迟",
  battery_remaining: "剩余电量",
  voltage_battery: "电池总电压",
  voltages: "电池电压",
  humidity_percent: "相对湿度",
  message_id: "消息 ID",
  sequence: "消息序号",
  severity: "严重程度",
  vendor_id: "设备编号",
  device_id: "设备 ID",
  device_type: "设备类型",
  remote_id: "Remote ID",
  uid: "PX4 UID",
  uid2: "PX4 UID2",
  gateway: "树莓派网关",
  dcdw_label: "设备标签",
  school_name: "所属学校"
};

export function buildTelemetryFrameGroups(
  source: JsonValue | null | undefined
): TelemetryFrameGroup[] {
  if (!isRecord(source)) return [];

  const groups: TelemetryFrameGroup[] = [];

  for (const [key, value] of Object.entries(source)) {
    if (key === "telemetry" && isRecord(value)) {
      for (const [childKey, childValue] of Object.entries(value)) {
        groups.push(createGroup(`telemetry.${childKey}`, childKey, childValue));
      }
      continue;
    }

    groups.push(createGroup(key, key, value));
  }

  return groups;
}

export function summarizeTelemetryFrame(
  source: JsonValue | null | undefined,
  groups = buildTelemetryFrameGroups(source)
): TelemetryFrameSummary {
  const entries = groups.flatMap((group) => group.entries);
  const serialized = JSON.stringify(source ?? {});

  return {
    fieldCount: entries.length,
    populatedCount: entries.filter((entry) => entry.type !== "null" && entry.type !== "empty").length,
    groupCount: groups.length,
    byteCount: new TextEncoder().encode(serialized).length
  };
}

export function matchesTelemetryEntry(
  entry: TelemetryFrameEntry,
  query: string
): boolean {
  const normalized = query.trim().toLocaleLowerCase();
  if (!normalized) return true;

  return [entry.path, entry.label, entry.displayValue, entry.unit ?? "", entry.type]
    .join(" ")
    .toLocaleLowerCase()
    .includes(normalized);
}

function createGroup(path: string, key: string, value: JsonValue): TelemetryFrameGroup {
  return {
    id: path,
    path,
    title: labelForKey(key),
    entries: flattenValue(value, path)
  };
}

function flattenValue(value: JsonValue, path: string): TelemetryFrameEntry[] {
  if (Array.isArray(value)) {
    if (value.length === 0) return [createEntry(path, value, "empty")];
    return value.flatMap((item, index) => flattenValue(item, `${path}[${index}]`));
  }

  if (isRecord(value)) {
    const children = Object.entries(value);
    if (children.length === 0) return [createEntry(path, value, "empty")];
    return children.flatMap(([key, child]) => flattenValue(child, `${path}.${key}`));
  }

  return [createEntry(path, value)];
}

function createEntry(
  path: string,
  value: JsonValue,
  forcedType?: TelemetryValueType
): TelemetryFrameEntry {
  const key = lastKey(path);
  return {
    path,
    label: labelForPath(path, key),
    value,
    displayValue: displayValue(value),
    unit: unitForPath(path),
    type: forcedType ?? valueType(value)
  };
}

function labelForPath(path: string, key: string): string {
  const arrayMatch = key.match(/^(.+)\[(\d+)\]$/);
  if (arrayMatch) {
    const base = LABELS[arrayMatch[1]] ?? readableKey(arrayMatch[1]);
    return `${base} ${Number(arrayMatch[2]) + 1}`;
  }
  return LABELS[key] ?? readableKey(key);
}

function labelForKey(key: string): string {
  return LABELS[key] ?? readableKey(key);
}

function readableKey(key: string): string {
  return key.replaceAll("_", " ");
}

function lastKey(path: string): string {
  return path.slice(path.lastIndexOf(".") + 1);
}

function displayValue(value: JsonValue): string {
  if (value === null) return "null";
  if (Array.isArray(value)) return value.length === 0 ? "[]" : JSON.stringify(value);
  if (isRecord(value)) return Object.keys(value).length === 0 ? "{}" : JSON.stringify(value);
  if (typeof value === "boolean") return value ? "true" : "false";
  return String(value);
}

function valueType(value: JsonValue): TelemetryValueType {
  if (value === null) return "null";
  if (typeof value === "number") return "number";
  if (typeof value === "boolean") return "boolean";
  return "string";
}

function unitForPath(path: string): string | null {
  const normalized = path.toLocaleLowerCase();
  if (/(^|\.)(roll|pitch|yaw|roll_deg|pitch_deg|yaw_deg|hdg)$/.test(normalized)) return "°";
  if (/temperature(_c)?$/.test(normalized)) return "℃";
  if (/(press_abs|pressure_hpa)$/.test(normalized)) return "hPa";
  if (/(pwm_us)(\[\d+\])?$/.test(normalized)) return "μs";
  if (/(battery_remaining|loss_rate_percent|packet_loss_pct|packet_loss_percent|humidity_percent)$/.test(normalized)) return "%";
  if (/(voltage_battery|voltages\[\d+\])$/.test(normalized)) return "V";
  if (/voltage_mv$/.test(normalized)) return "mV";
  if (/rssi(_dbm)?$/.test(normalized)) return "dBm";
  if (/(latency_ms|interval_ms|heartbeat_interval_ms|publish_interval_ms)$/.test(normalized)) return "ms";
  if (/(altitude_m|alt|alt_ellipsoid|h_acc|v_acc)$/.test(normalized)) return "m";
  return null;
}

function isRecord(value: JsonValue | null | undefined): value is { [key: string]: JsonValue } {
  return Boolean(value) && typeof value === "object" && !Array.isArray(value);
}
