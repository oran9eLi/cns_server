import type { AppConfig } from "../config/appConfig.js";

export type LogLevel = "debug" | "info" | "warn" | "error";

const levelPriority: Record<LogLevel, number> = {
  debug: 10,
  info: 20,
  warn: 30,
  error: 40
};

export interface Logger {
  debug(message: string, fields?: Record<string, unknown>): void;
  info(message: string, fields?: Record<string, unknown>): void;
  warn(message: string, fields?: Record<string, unknown>): void;
  error(message: string, fields?: Record<string, unknown>): void;
}

export function createLogger(config: AppConfig): Logger {
  const minimumLevel = config.logging.level;

  function write(level: LogLevel, message: string, fields: Record<string, unknown> = {}) {
    if (levelPriority[level] < levelPriority[minimumLevel]) {
      return;
    }

    const entry = {
      time: new Date().toISOString(),
      level,
      message,
      ...redactSensitiveFields(fields)
    };

    const line = JSON.stringify(entry);
    if (level === "error" || level === "warn") {
      console.error(line);
      return;
    }

    console.log(line);
  }

  return {
    debug: (message, fields) => write("debug", message, fields),
    info: (message, fields) => write("info", message, fields),
    warn: (message, fields) => write("warn", message, fields),
    error: (message, fields) => write("error", message, fields)
  };
}

function redactSensitiveFields(value: Record<string, unknown>): Record<string, unknown> {
  return Object.fromEntries(
    Object.entries(value).map(([key, fieldValue]) => [
      key,
      isSensitiveKey(key) ? "[REDACTED]" : fieldValue
    ])
  );
}

function isSensitiveKey(key: string): boolean {
  const normalized = key.toLowerCase();
  return (
    normalized.includes("password") ||
    normalized.includes("passwd") ||
    normalized.includes("secret") ||
    normalized.includes("token") ||
    normalized.includes("credential")
  );
}
