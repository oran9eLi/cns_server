-- 学校是设备的归属单位。
CREATE TABLE schools (
  school_id BIGSERIAL PRIMARY KEY,
  school_name TEXT NOT NULL UNIQUE,
  created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
);

-- 设备以厂商编号作为跨系统稳定标识。
CREATE TABLE devices (
  vendor_id VARCHAR(20) PRIMARY KEY,
  school_id BIGINT NOT NULL REFERENCES schools(school_id),
  dcdw_label TEXT,
  model_version TEXT NOT NULL DEFAULT 'CNS v1.0',
  provisioned_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
  UNIQUE (school_id, dcdw_label)
);

-- 每台设备只保存一份最新状态与最新遥测。
CREATE TABLE device_latest_states (
  vendor_id VARCHAR(20) PRIMARY KEY REFERENCES devices(vendor_id),
  status TEXT NOT NULL CHECK (status IN ('online', 'offline')),
  last_seen_at TIMESTAMPTZ,
  latest_telemetry JSONB,
  telemetry_received_at TIMESTAMPTZ,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
);

-- 命令来源是应用层白名单；设备来源必须与对应设备使用相同标识。
CREATE TABLE command_sources (
  source_id VARCHAR(64) PRIMARY KEY CHECK (source_id ~ '^[A-Za-z0-9._-]+$'),
  source_kind TEXT NOT NULL CHECK (source_kind IN ('device', 'host_app', 'control_center')),
  device_vendor_id VARCHAR(20) UNIQUE REFERENCES devices(vendor_id),
  enabled BOOLEAN NOT NULL DEFAULT true,
  created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
  CHECK (
    (source_kind = 'device' AND device_vendor_id IS NOT NULL AND source_id = device_vendor_id)
    OR
    (source_kind IN ('host_app', 'control_center') AND device_vendor_id IS NULL)
  )
);

-- 命令保留来源幂等键；无法完成寻址的请求允许目标设备为空。
CREATE TABLE commands (
  command_id UUID PRIMARY KEY,
  source_id VARCHAR(64) NOT NULL REFERENCES command_sources(source_id),
  request_id VARCHAR(128) NOT NULL,
  command_type TEXT NOT NULL CHECK (command_type IN ('config', 'control')),
  target_vendor_id VARCHAR(20) REFERENCES devices(vendor_id),
  request_payload JSONB NOT NULL,
  status TEXT NOT NULL CHECK (status IN ('pending', 'dispatched', 'in_progress', 'succeeded', 'failed', 'timeout', 'delivery_uncertain')),
  error_code TEXT,
  error_message TEXT,
  device_ack JSONB,
  created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
  dispatched_at TIMESTAMPTZ,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
  completed_at TIMESTAMPTZ,
  UNIQUE (source_id, request_id)
);
