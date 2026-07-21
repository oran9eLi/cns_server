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
  vendor_id: string;
  school_name: string;
  dcdw_label: string | null;
  model_version: string;
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
            d.vendor_id,
            s.name as school_name,
            d.dcdw_label,
            d.model_version,
            d.status,
            d.provisioned_at,
            d.last_seen_at,
            d.telemetry_received_at,
            d.latest_telemetry,
            d.degraded
          from devices d
          join schools s on s.id = d.school_id
          ${where}
          order by d.school_id asc, d.vendor_id asc
        `,
        values
      );

      return DeviceListResponseSchema.shape.items.parse(result.rows.map(toSummary));
    },
    async get(vendorId) {
      const result = await pool.query<DeviceRow>(
        `
          select
            d.vendor_id,
            s.name as school_name,
            d.dcdw_label,
            d.model_version,
            d.status,
            d.provisioned_at,
            d.last_seen_at,
            d.telemetry_received_at,
            d.latest_telemetry,
            d.degraded
          from devices d
          join schools s on s.id = d.school_id
          where d.vendor_id = $1
        `,
        [vendorId]
      );

      const row = result.rows[0];
      return row ? toDetail(row) : null;
    },
    async updateTelemetry(vendorId, telemetry) {
      const result = await pool.query<DeviceRow>(
        `
          update devices
          set
            latest_telemetry = $2::jsonb,
            telemetry_received_at = now(),
            last_seen_at = now(),
            status = 'online',
            degraded = false,
            updated_at = now()
          where vendor_id = $1
          returning
            vendor_id,
            (select name from schools where id = devices.school_id) as school_name,
            dcdw_label,
            model_version,
            status,
            provisioned_at,
            last_seen_at,
            telemetry_received_at,
            latest_telemetry,
            degraded
        `,
        [vendorId, JSON.stringify(telemetry)]
      );

      const row = result.rows[0];
      return row ? toDetail(row) : null;
    },
    async setMotorPwm(vendorId, motorPwm) {
      const result = await pool.query<DeviceRow>(
        `
          update devices
          set
            latest_telemetry = jsonb_set(
              coalesce(latest_telemetry, '{}'::jsonb),
              '{motors,pwm}',
              $2::jsonb,
              true
            ),
            telemetry_received_at = now(),
            updated_at = now()
          where vendor_id = $1
          returning
            vendor_id,
            (select name from schools where id = devices.school_id) as school_name,
            dcdw_label,
            model_version,
            status,
            provisioned_at,
            last_seen_at,
            telemetry_received_at,
            latest_telemetry,
            degraded
        `,
        [vendorId, JSON.stringify(motorPwm)]
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
      lower(d.vendor_id) like $${values.length}
      or lower(coalesce(d.dcdw_label, '')) like $${values.length}
      or lower(s.name) like $${values.length}
      or lower(d.model_version) like $${values.length}
    )`);
  }

  if (query.school_name) {
    values.push(query.school_name);
    clauses.push(`s.name = $${values.length}`);
  }

  if (query.status) {
    values.push(query.status);
    clauses.push(`d.status = $${values.length}`);
  }

  return {
    where: clauses.length ? `where ${clauses.join(" and ")}` : "",
    values
  };
}

function toSummary(row: DeviceRow): DeviceSummary {
  return {
    vendor_id: row.vendor_id,
    school_name: row.school_name,
    dcdw_label: row.dcdw_label,
    model_version: row.model_version,
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
