import { Pool, type PoolConfig } from "pg";

import type { DependencyStatus } from "@cns/backend-protocol";

import type { CommandRecord, CommandStore } from "./commandStore.js";

export function createPostgresCommandStore(config: PoolConfig): CommandStore {
  const pool = new Pool(config);

  return {
    async create(record) {
      await pool.query(
        `
          insert into command_requests (
            vendor_id,
            session_id,
            client_request_id,
            command_type,
            command,
            parameters,
            status,
            business_status,
            error,
            submitted_at,
            updated_at
          )
          values ($1, $2, $3, $4, $5, $6::jsonb, $7, $8, $9::jsonb, $10, $11)
          on conflict (session_id, client_request_id) do update
          set
            status = excluded.status,
            business_status = excluded.business_status,
            error = excluded.error,
            updated_at = excluded.updated_at
        `,
        [
          record.vendor_id,
          record.session_id,
          record.client_request_id,
          record.command_type,
          record.command,
          JSON.stringify(record.parameters),
          record.status,
          record.business_status,
          record.error ? JSON.stringify(record.error) : null,
          record.submitted_at,
          record.updated_at
        ]
      );
    },
    async updateStatus(sessionId, clientRequestId, status, businessStatus, error) {
      await pool.query(
        `
          update command_requests
          set
            status = $3,
            business_status = $4,
            error = $5::jsonb,
            updated_at = now()
          where session_id = $1 and client_request_id = $2
        `,
        [
          sessionId,
          clientRequestId,
          status,
          businessStatus,
          error ? JSON.stringify(error) : null
        ]
      );
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
