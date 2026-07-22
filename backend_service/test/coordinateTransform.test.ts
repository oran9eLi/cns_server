import { describe, expect, it } from "vitest";

import {
  enrichTelemetryCoordinates,
  wgs84ToGcj02
} from "../src/telemetry/coordinateTransform.js";

describe("coordinate transform", () => {
  it("converts a mainland WGS84 coordinate to GCJ-02", () => {
    const result = wgs84ToGcj02(116.404, 39.915);

    expect(result.longitude).toBeCloseTo(116.4102444992, 8);
    expect(result.latitude).toBeCloseTo(39.9164042815, 8);
  });

  it("does not offset coordinates outside mainland China", () => {
    expect(wgs84ToGcj02(-0.1276, 51.5072)).toEqual({
      longitude: -0.1276,
      latitude: 51.5072
    });
  });

  it("preserves WGS84 and adds canonical GCJ-02 telemetry fields", () => {
    const source = {
      telemetry: {
        gps: {
          latitude: 39.915,
          longitude: 116.404,
          fix_valid: true
        }
      }
    };
    const result = enrichTelemetryCoordinates(source);
    const telemetry = result?.telemetry as Record<string, unknown>;
    const position = telemetry.position as Record<string, unknown>;

    expect(position).toMatchObject({
      latitude_wgs84: 39.915,
      longitude_wgs84: 116.404,
      source_coordinate_system: "WGS84",
      display_coordinate_system: "GCJ-02",
      coordinate_conversion: "backend_local"
    });
    expect(position.longitude_gcj02).toBeCloseTo(116.4102444992, 8);
    expect(position.latitude_gcj02).toBeCloseTo(39.9164042815, 8);
    expect(source.telemetry.gps).not.toHaveProperty("longitude_gcj02");
  });

  it("does not convert an explicitly invalid GPS fix", () => {
    const result = enrichTelemetryCoordinates({
      telemetry: {
        position: {
          latitude: 39.915,
          longitude: 116.404,
          fix_valid: false
        }
      }
    });
    const telemetry = result?.telemetry as Record<string, unknown>;
    const position = telemetry.position as Record<string, unknown>;

    expect(position).not.toHaveProperty("latitude_gcj02");
  });
  it("supports the cns_rpi numeric GPS fix type", () => {
    const valid = enrichTelemetryCoordinates({
      telemetry: { gps: { lat: 39.915, lon: 116.404, fix_type: 3 } }
    });
    const validPosition = (valid?.telemetry as Record<string, unknown>).position as Record<string, unknown>;
    expect(validPosition.latitude_gcj02).toBeCloseTo(39.9164042815, 8);

    const invalid = enrichTelemetryCoordinates({
      telemetry: { gps: { lat: 39.915, lon: 116.404, fix_type: 1 } }
    });
    const invalidTelemetry = invalid?.telemetry as Record<string, unknown>;
    expect(invalidTelemetry.position).toBeUndefined();
  });

  it("uses cns_rpi global_position when GPS coordinates are unavailable", () => {
    const result = enrichTelemetryCoordinates({
      telemetry: { global_position: { lat: 39.915, lon: 116.404, alt: 18.2, hdg: 156.4 } }
    });
    const position = (result?.telemetry as Record<string, unknown>).position as Record<string, unknown>;
    expect(position).toMatchObject({ latitude_wgs84: 39.915, longitude_wgs84: 116.404 });
    expect(position.latitude_gcj02).toBeCloseTo(39.9164042815, 8);
  });
});
