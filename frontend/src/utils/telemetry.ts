import type { JsonValue } from "@cns/backend-protocol";

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
