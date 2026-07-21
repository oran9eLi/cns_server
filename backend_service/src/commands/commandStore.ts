import type {
  CommandStatus,
  DependencyStatus,
  DeviceCommandRequest,
  ErrorPayload
} from "@cns/backend-protocol";

export type CommandRecord = {
  vendor_id: string;
  session_id: string;
  client_request_id: string;
  command_type: DeviceCommandRequest["type"];
  command: string | null;
  parameters: DeviceCommandRequest["parameters"];
  status: CommandStatus;
  business_status: string | null;
  error: ErrorPayload | null;
  submitted_at: string;
  updated_at: string;
};

export interface CommandStore {
  create(record: CommandRecord): Promise<void>;
  updateStatus(
    sessionId: string,
    clientRequestId: string,
    status: CommandStatus,
    businessStatus: string | null,
    error: ErrorPayload | null
  ): Promise<void>;
  dependencyStatus(): Promise<DependencyStatus>;
  close(): Promise<void>;
}

export function createInMemoryCommandStore(): CommandStore {
  const records = new Map<string, CommandRecord>();

  return {
    async create(record) {
      records.set(key(record.session_id, record.client_request_id), structuredClone(record));
    },
    async updateStatus(sessionId, clientRequestId, status, businessStatus, error) {
      const record = records.get(key(sessionId, clientRequestId));
      if (!record) return;
      record.status = status;
      record.business_status = businessStatus;
      record.error = error;
      record.updated_at = new Date().toISOString();
    },
    async dependencyStatus() {
      return "not_configured";
    },
    async close() {
      return undefined;
    }
  };
}

export function toCommandRecord(
  vendorId: string,
  request: DeviceCommandRequest,
  submittedAt: string
): CommandRecord {
  return {
    vendor_id: vendorId,
    session_id: request.session_id,
    client_request_id: request.client_request_id,
    command_type: request.type,
    command: request.type === "control" ? request.command : null,
    parameters: request.parameters,
    status: "submitted",
    business_status: "submitted",
    error: null,
    submitted_at: submittedAt,
    updated_at: submittedAt
  };
}

function key(sessionId: string, clientRequestId: string): string {
  return `${sessionId}:${clientRequestId}`;
}

export function businessStatusFor(status: CommandStatus): string | null {
  if (status === "succeeded") return "accepted";
  if (status === "failed") return "rejected";
  if (status === "timeout") return "timeout";
  if (status === "delivery_uncertain") return "delivery_uncertain";
  return status;
}
