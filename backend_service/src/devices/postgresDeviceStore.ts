import { Pool, type PoolConfig } from "pg";

import {
  DeviceDetailSchema,
  DeviceListResponseSchema,
  type DependencyStatus,
  type DeviceDetail,
  type DeviceListQuery,
  type DeviceSummary,
  type JsonValue
} from "@cns/backend-protocol";

import type { DeviceStore } from "./deviceStore.js";

type DeviceRow = {
  device_id: string;
  device_type: "cns_box" | "flight_controller";
  school_name: string | null;
  dcdw_label: string | null;
  model_version: string;
  capabilities: string[] | null;
  product: Record<string, JsonValue> | null;
  version: Record<string, JsonValue> | null;
  status: "online" | "offline";
  provisioned_at: Date | string | null;
  last_seen_at: Date | string | null;
  telemetry_received_at: Date | string | null;
  latest_telemetry: Record<string, JsonValue> | null;
  degraded: boolean;
};

export function createPostgresDeviceStore(config: PoolConfig): DeviceStore {
  const pool = new Pool(config);

  return {
    async list(query) {
      const { where, values } = buildListWhere(query);
      const result = await pool.query<DeviceRow>(
        `
          select
            d.device_id,
            d.device_type,
            s.school_name,
            d.dcdw_label,
            d.model_version,
            d.capabilities,
            d.product,
            d.version,
            d.provisioned_at,
            state.status,
            state.last_seen_at,
            state.telemetry_received_at,
            state.latest_telemetry,
            false as degraded
          from devices d
          left join schools s on s.school_id = d.school_id
          join device_latest_states state on state.device_id = d.device_id
          ${where}
          order by d.school_id asc, d.device_id asc
        `,
        values
      );

      return DeviceListResponseSchema.shape.items.parse(result.rows.map(toSummary));
    },
    async get(deviceId) {
      const result = await pool.query<DeviceRow>(
        `
          select
            d.device_id,
            d.device_type,
            s.school_name,
            d.dcdw_label,
            d.model_version,
            d.capabilities,
            d.product,
            d.version,
            d.provisioned_at,
            state.status,
            state.last_seen_at,
            state.telemetry_received_at,
            state.latest_telemetry,
            false as degraded
          from devices d
          left join schools s on s.school_id = d.school_id
          join device_latest_states state on state.device_id = d.device_id
          where d.device_id = $1
        `,
        [deviceId]
      );

      const row = result.rows[0];
      return row ? toDetail(row) : null;
    },
    async dependencyStatus(): Promise<DependencyStatus> {
      try {
        await pool.query("select 1");
        return "ready";
      } catch {
        return "unavailable";
      }
    },
    async close() {
      await pool.end();
    }
  };
}

function buildListWhere(query: DeviceListQuery): { where: string; values: unknown[] } {
  const clauses: string[] = [];
  const values: unknown[] = [];

  if (query.keyword) {
    values.push(`%${query.keyword.toLowerCase()}%`);
    clauses.push(`(
      lower(d.device_id) like $${values.length}
      or lower(coalesce(d.dcdw_label, '')) like $${values.length}
      or lower(coalesce(s.school_name, '')) like $${values.length}
      or lower(d.model_version) like $${values.length}
    )`);
  }

  if (query.school_name) {
    values.push(query.school_name);
    clauses.push(`s.school_name = $${values.length}`);
  }

  if (query.status) {
    values.push(query.status);
    clauses.push(`state.status = $${values.length}`);
  }

  return {
    where: clauses.length ? `where ${clauses.join(" and ")}` : "",
    values
  };
}

function toSummary(row: DeviceRow): DeviceSummary {
  return {
    device_id: row.device_id,
    device_type: row.device_type,
    school_name: row.school_name,
    dcdw_label: row.dcdw_label,
    model_version: row.model_version,
    capabilities: row.capabilities,
    product: row.product,
    version: row.version,
    status: row.status,
    last_seen_at: toDateTimeString(row.last_seen_at),
    telemetry_received_at: toDateTimeString(row.telemetry_received_at),
    degraded: row.degraded
  };
}

function toDetail(row: DeviceRow): DeviceDetail {
  return DeviceDetailSchema.parse({
    ...toSummary(row),
    provisioned_at: toDateTimeString(row.provisioned_at),
    latest_telemetry: row.latest_telemetry
  });
}

function toDateTimeString(value: Date | string | null): string | null {
  if (!value) return null;
  if (value instanceof Date) return value.toISOString();
  return new Date(value).toISOString();
}
