import type { JsonValue } from "@cns/backend-protocol";

export interface TelemetryView {
  attitude: {
    roll: number | null;
    pitch: number | null;
    yaw: number | null;
  };
  environment: {
    temperature: number | null;
    pressure: number | null;
    altitude: number | null;
  };
  link: {
    rssi: number | null;
    packetLoss: number | null;
    latency: number | null;
  };
  motors: {
    pwm: Array<number | null>;
  };
  battery: {
    remaining: number | null;
    voltage: number | null;
  };
}

export function mapTelemetry(source: JsonValue | undefined): TelemetryView {
  const realBatteryVoltages = readArray(source, "telemetry.battery.voltages");

  return {
    attitude: {
      roll: firstNumber(source, "telemetry.attitude.roll", "attitude.roll_deg"),
      pitch: firstNumber(source, "telemetry.attitude.pitch", "attitude.pitch_deg"),
      yaw: firstNumber(source, "telemetry.attitude.yaw", "attitude.yaw_deg")
    },
    environment: {
      temperature: firstNumber(source, "telemetry.pressure.temperature", "environment.temperature_c"),
      pressure: firstNumber(source, "telemetry.pressure.press_abs", "environment.pressure_hpa"),
      altitude: readNumber(source, "environment.altitude_m")
    },
    link: {
      rssi: readNumber(source, "link.rssi_dbm"),
      packetLoss: firstNumber(source, "telemetry.lora.loss_rate_percent", "link.packet_loss_pct"),
      latency: readNumber(source, "link.latency_ms")
    },
    motors: {
      pwm: normalizePwm(
        readArray(source, "telemetry.motor.pwm_us") ?? readArray(source, "motors.pwm")
      )
    },
    battery: {
      remaining: readNumber(source, "telemetry.battery.battery_remaining"),
      voltage:
        readNumber(source, "telemetry.battery.voltage_battery") ??
        finiteNumber(realBatteryVoltages?.[0])
    }
  };
}

export function readNumber(source: JsonValue | undefined, path: string): number | null {
  const value = readValue(source, path);
  return typeof value === "number" && Number.isFinite(value) ? value : null;
}

export function readArray(source: JsonValue | undefined, path: string): JsonValue[] | null {
  const value = readValue(source, path);
  return Array.isArray(value) ? value : null;
}

export function formatNumber(value: number | null, suffix = "", digits = 1): string {
  if (value === null) return "--";
  return `${value.toFixed(digits)}${suffix}`;
}

export function formatDateTime(value: string | null | undefined): string {
  if (!value) return "--";
  return new Intl.DateTimeFormat("zh-CN", {
    month: "2-digit",
    day: "2-digit",
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit"
  }).format(new Date(value));
}

function readValue(source: JsonValue | undefined, path: string): JsonValue | undefined {
  return path.split(".").reduce<JsonValue | undefined>((current, part) => {
    if (!current || typeof current !== "object" || Array.isArray(current)) return undefined;
    return current[part];
  }, source);
}

function firstNumber(source: JsonValue | undefined, ...paths: string[]): number | null {
  for (const path of paths) {
    const value = readNumber(source, path);
    if (value !== null) return value;
  }
  return null;
}

function normalizePwm(values: JsonValue[] | null): Array<number | null> {
  return Array.from({ length: 4 }, (_, index) => finiteNumber(values?.[index]));
}

function finiteNumber(value: JsonValue | undefined): number | null {
  return typeof value === "number" && Number.isFinite(value) ? value : null;
}
