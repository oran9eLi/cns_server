-- CNS backend_service V1 persistence schema draft.
-- The service reads device snapshots and command state from these tables.
-- route_service remains the authority for device routing and command execution.

create table if not exists schools (
  id bigserial primary key,
  name text not null unique,
  created_at timestamptz not null default now()
);

create table if not exists devices (
  vendor_id text primary key,
  school_id bigint not null references schools(id),
  dcdw_label text,
  model_version text not null,
  status text not null check (status in ('online', 'offline')),
  provisioned_at timestamptz,
  last_seen_at timestamptz,
  telemetry_received_at timestamptz,
  latest_telemetry jsonb,
  degraded boolean not null default false,
  updated_at timestamptz not null default now()
);

create index if not exists devices_school_id_idx on devices(school_id);
create index if not exists devices_status_idx on devices(status);
create index if not exists devices_last_seen_at_idx on devices(last_seen_at desc);

create table if not exists command_requests (
  id bigserial primary key,
  vendor_id text not null references devices(vendor_id),
  session_id text not null,
  client_request_id uuid not null,
  command_type text not null check (command_type in ('config', 'control')),
  command text,
  parameters jsonb not null,
  status text not null,
  business_status text,
  error jsonb,
  submitted_at timestamptz not null default now(),
  updated_at timestamptz not null default now(),
  unique (session_id, client_request_id)
);

create index if not exists command_requests_vendor_id_idx on command_requests(vendor_id);
create index if not exists command_requests_updated_at_idx on command_requests(updated_at desc);
