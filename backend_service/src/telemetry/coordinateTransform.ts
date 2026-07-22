const PI = Math.PI;
const EARTH_SEMI_MAJOR_AXIS = 6378245.0;
const ECCENTRICITY_SQUARED = 0.006693421622965943;

type JsonRecord = Record<string, unknown>;

export type Coordinate = {
  latitude: number;
  longitude: number;
};

export function wgs84ToGcj02(longitude: number, latitude: number): Coordinate {
  if (!isValidCoordinate(longitude, latitude) || isOutsideMainlandChina(longitude, latitude)) {
    return { latitude, longitude };
  }

  let latitudeOffset = transformLatitude(longitude - 105, latitude - 35);
  let longitudeOffset = transformLongitude(longitude - 105, latitude - 35);
  const latitudeRadians = latitude / 180 * PI;
  let magic = Math.sin(latitudeRadians);
  magic = 1 - ECCENTRICITY_SQUARED * magic * magic;
  const sqrtMagic = Math.sqrt(magic);

  latitudeOffset = latitudeOffset * 180
    / ((EARTH_SEMI_MAJOR_AXIS * (1 - ECCENTRICITY_SQUARED)) / (magic * sqrtMagic) * PI);
  longitudeOffset = longitudeOffset * 180
    / (EARTH_SEMI_MAJOR_AXIS / sqrtMagic * Math.cos(latitudeRadians) * PI);

  return {
    latitude: latitude + latitudeOffset,
    longitude: longitude + longitudeOffset
  };
}

export function enrichTelemetryCoordinates(
  source: Record<string, unknown> | null
): Record<string, unknown> | null {
  if (!source) return null;

  const output = structuredClone(source);
  const sourcePosition = findSourcePosition(output);
  if (!sourcePosition || sourcePosition.fixValid === false) return output;

  const telemetry = ensureRecord(output, "telemetry");
  const position = ensureRecord(telemetry, "position");
  const coordinateSystem = sourcePosition.coordinateSystem.toUpperCase().replaceAll("_", "-");

  if (coordinateSystem.includes("GCJ")) {
    position.latitude_gcj02 = sourcePosition.latitude;
    position.longitude_gcj02 = sourcePosition.longitude;
    position.display_coordinate_system = "GCJ-02";
    position.coordinate_conversion = "source_gcj02";
    return output;
  }

  const converted = wgs84ToGcj02(sourcePosition.longitude, sourcePosition.latitude);
  position.latitude_wgs84 = sourcePosition.latitude;
  position.longitude_wgs84 = sourcePosition.longitude;
  position.latitude_gcj02 = converted.latitude;
  position.longitude_gcj02 = converted.longitude;
  position.source_coordinate_system = "WGS84";
  position.display_coordinate_system = "GCJ-02";
  position.coordinate_conversion = "backend_local";
  return output;
}

function findSourcePosition(source: JsonRecord): {
  latitude: number;
  longitude: number;
  coordinateSystem: string;
  fixValid: boolean | null;
} | null {
  const telemetry = asRecord(source.telemetry);
  const candidates = [
    asRecord(telemetry?.position),
    asRecord(telemetry?.gps),
    asRecord(telemetry?.global_position),
    asRecord(telemetry?.location),
    asRecord(source.position),
    asRecord(source.gps),
    asRecord(source.global_position),
    asRecord(source.location),
    source
  ].filter((value): value is JsonRecord => Boolean(value));

  for (const candidate of candidates) {
    const latitude = coordinateNumber(candidate, [
      "latitude_wgs84", "latitude_deg", "latitude", "lat_deg", "lat"
    ], 90);
    const longitude = coordinateNumber(candidate, [
      "longitude_wgs84", "longitude_deg", "longitude", "lon_deg", "lng_deg", "lon", "lng"
    ], 180);
    if (latitude === null || longitude === null || (latitude === 0 && longitude === 0)) continue;

    return {
      latitude,
      longitude,
      coordinateSystem: stringValue(candidate, [
        "coordinate_system", "source_coordinate_system", "coord_type", "crs"
      ]) ?? "WGS84",
      fixValid: fixValidity(candidate)
    };
  }

  return null;
}

function coordinateNumber(record: JsonRecord, keys: string[], maximum: number): number | null {
  for (const key of keys) {
    const raw = record[key];
    const value = typeof raw === "number"
      ? raw
      : typeof raw === "string" && raw.trim() !== ""
        ? Number(raw)
        : Number.NaN;
    if (!Number.isFinite(value)) continue;
    const normalized = Math.abs(value) > maximum && Math.abs(value) <= maximum * 10_000_000
      ? value / 10_000_000
      : value;
    if (Math.abs(normalized) <= maximum) return normalized;
  }
  return null;
}

function fixValidity(record: JsonRecord): boolean | null {
  for (const key of ["fix_valid", "valid", "has_fix"]) {
    const value = record[key];
    if (typeof value === "boolean") return value;
    if (value === 0 || value === "0" || value === "false") return false;
    if (value === 1 || value === "1" || value === "true") return true;
  }

  for (const key of ["fix_type", "fix", "gps_fix"]) {
    const value = record[key];
    if (typeof value === "number" && Number.isFinite(value)) return value >= 2;
    if (typeof value === "string" && value.trim() !== "") {
      const numericValue = Number(value);
      if (Number.isFinite(numericValue)) return numericValue >= 2;
    }
  }

  const fixType = stringValue(record, ["fix_type", "fix", "gps_fix"]);
  if (!fixType) return null;
  const normalized = fixType.trim().toLowerCase();
  if (["none", "no_fix", "nofix", "invalid", "offline"].includes(normalized)) return false;
  return true;
}

function stringValue(record: JsonRecord, keys: string[]): string | null {
  for (const key of keys) {
    const value = record[key];
    if (typeof value === "string" && value.trim() !== "") return value;
  }
  return null;
}

function ensureRecord(parent: JsonRecord, key: string): JsonRecord {
  const current = asRecord(parent[key]);
  if (current) return current;
  const created: JsonRecord = {};
  parent[key] = created;
  return created;
}

function asRecord(value: unknown): JsonRecord | null {
  return value !== null && typeof value === "object" && !Array.isArray(value)
    ? value as JsonRecord
    : null;
}

function isValidCoordinate(longitude: number, latitude: number): boolean {
  return Number.isFinite(longitude)
    && Number.isFinite(latitude)
    && longitude >= -180
    && longitude <= 180
    && latitude >= -90
    && latitude <= 90;
}

function isOutsideMainlandChina(longitude: number, latitude: number): boolean {
  return longitude < 72.004
    || longitude > 137.8347
    || latitude < 0.8293
    || latitude > 55.8271;
}

function transformLatitude(longitude: number, latitude: number): number {
  let result = -100 + 2 * longitude + 3 * latitude + 0.2 * latitude * latitude
    + 0.1 * longitude * latitude + 0.2 * Math.sqrt(Math.abs(longitude));
  result += (20 * Math.sin(6 * longitude * PI) + 20 * Math.sin(2 * longitude * PI)) * 2 / 3;
  result += (20 * Math.sin(latitude * PI) + 40 * Math.sin(latitude / 3 * PI)) * 2 / 3;
  result += (160 * Math.sin(latitude / 12 * PI) + 320 * Math.sin(latitude * PI / 30)) * 2 / 3;
  return result;
}

function transformLongitude(longitude: number, latitude: number): number {
  let result = 300 + longitude + 2 * latitude + 0.1 * longitude * longitude
    + 0.1 * longitude * latitude + 0.1 * Math.sqrt(Math.abs(longitude));
  result += (20 * Math.sin(6 * longitude * PI) + 20 * Math.sin(2 * longitude * PI)) * 2 / 3;
  result += (20 * Math.sin(longitude * PI) + 40 * Math.sin(longitude / 3 * PI)) * 2 / 3;
  result += (150 * Math.sin(longitude / 12 * PI) + 300 * Math.sin(longitude / 30 * PI)) * 2 / 3;
  return result;
}
