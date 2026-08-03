-- 当前环境只有测试数据：旧 PX4U1/PX4U2 主键无法映射到 Basic ID，直接删除。
DELETE FROM commands
WHERE target_vendor_id IN (
  SELECT vendor_id FROM devices
  WHERE vendor_id LIKE 'PX4U1-%' OR vendor_id LIKE 'PX4U2-%'
)
OR source_id IN (
  SELECT source_id FROM command_sources
  WHERE device_vendor_id LIKE 'PX4U1-%' OR device_vendor_id LIKE 'PX4U2-%'
);

DELETE FROM command_sources
WHERE device_vendor_id LIKE 'PX4U1-%' OR device_vendor_id LIKE 'PX4U2-%';

DELETE FROM device_latest_states
WHERE vendor_id LIKE 'PX4U1-%' OR vendor_id LIKE 'PX4U2-%';

DELETE FROM devices
WHERE vendor_id LIKE 'PX4U1-%' OR vendor_id LIKE 'PX4U2-%';

ALTER TABLE device_latest_states
  DROP CONSTRAINT device_latest_states_vendor_id_fkey;
ALTER TABLE command_sources
  DROP CONSTRAINT command_sources_device_vendor_id_fkey;
ALTER TABLE commands
  DROP CONSTRAINT commands_target_vendor_id_fkey;

ALTER TABLE devices RENAME COLUMN vendor_id TO device_id;
ALTER TABLE device_latest_states RENAME COLUMN vendor_id TO device_id;
ALTER TABLE command_sources RENAME COLUMN device_vendor_id TO device_id;
ALTER TABLE commands RENAME COLUMN target_vendor_id TO target_device_id;

ALTER TABLE devices ALTER COLUMN device_id TYPE VARCHAR(20);
ALTER TABLE device_latest_states ALTER COLUMN device_id TYPE VARCHAR(20);
ALTER TABLE command_sources ALTER COLUMN device_id TYPE VARCHAR(20);
ALTER TABLE commands ALTER COLUMN target_device_id TYPE VARCHAR(20);

ALTER TABLE devices ADD COLUMN capabilities JSONB;
ALTER TABLE devices ADD COLUMN product JSONB;
ALTER TABLE devices ADD COLUMN version JSONB;

ALTER TABLE device_latest_states
  ADD CONSTRAINT device_latest_states_device_id_fkey
  FOREIGN KEY (device_id) REFERENCES devices(device_id);
ALTER TABLE command_sources
  ADD CONSTRAINT command_sources_device_id_fkey
  FOREIGN KEY (device_id) REFERENCES devices(device_id);
ALTER TABLE commands
  ADD CONSTRAINT commands_target_device_id_fkey
  FOREIGN KEY (target_device_id) REFERENCES devices(device_id);
