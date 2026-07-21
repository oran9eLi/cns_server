import {
  CommandAcceptedResponseSchema,
  DeviceDetailResponseSchema,
  DeviceListResponseSchema,
  ErrorResponseSchema,
  HealthResponseSchema,
  type CommandAcceptedResponse,
  type DeviceCommandRequest,
  type DeviceDetailResponse,
  type DeviceListQuery,
  type DeviceListResponse,
  type HealthResponse
} from "@cns/backend-protocol";

export async function getHealth(): Promise<HealthResponse> {
  return parseJson("/api/health", HealthResponseSchema.parse);
}

export async function getDevices(query: DeviceListQuery): Promise<DeviceListResponse> {
  const params = new URLSearchParams();
  if (query.keyword) params.set("keyword", query.keyword);
  if (query.school_name) params.set("school_name", query.school_name);
  if (query.status) params.set("status", query.status);
  const suffix = params.size > 0 ? `?${params.toString()}` : "";
  return parseJson(`/api/devices${suffix}`, DeviceListResponseSchema.parse);
}

export async function getDevice(vendorId: string): Promise<DeviceDetailResponse> {
  return parseJson(`/api/devices/${encodeURIComponent(vendorId)}`, DeviceDetailResponseSchema.parse);
}

export async function postCommand(vendorId: string, body: DeviceCommandRequest): Promise<CommandAcceptedResponse> {
  return parseJson(`/api/devices/${encodeURIComponent(vendorId)}/commands`, CommandAcceptedResponseSchema.parse, {
    method: "POST",
    headers: {
      "content-type": "application/json"
    },
    body: JSON.stringify(body)
  });
}

async function parseJson<T>(url: string, parse: (value: unknown) => T, init?: RequestInit): Promise<T> {
  const response = await fetch(url, init);
  const data = await response.json();

  if (!response.ok) {
    const parsed = ErrorResponseSchema.safeParse(data);
    throw new Error(parsed.success ? parsed.data.error.message : `请求失败：${response.status}`);
  }

  return parse(data);
}
