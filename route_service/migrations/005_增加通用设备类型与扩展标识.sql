-- 主控箱继续使用 20 位厂商编号；真实 PX4 使用 PX4U2-/PX4U1- 前缀设备标识。
-- 先解除外键再同步扩宽，最后恢复约束，迁移可在已有数据上执行。
ALTER TABLE device_latest_states
  DROP CONSTRAINT device_latest_states_vendor_id_fkey;
ALTER TABLE command_sources
  DROP CONSTRAINT command_sources_device_vendor_id_fkey;
ALTER TABLE commands
  DROP CONSTRAINT commands_target_vendor_id_fkey;

ALTER TABLE devices ALTER COLUMN vendor_id TYPE VARCHAR(64);
ALTER TABLE device_latest_states ALTER COLUMN vendor_id TYPE VARCHAR(64);
ALTER TABLE command_sources ALTER COLUMN device_vendor_id TYPE VARCHAR(64);
ALTER TABLE commands ALTER COLUMN target_vendor_id TYPE VARCHAR(64);

ALTER TABLE command_sources
  DROP CONSTRAINT command_sources_source_id_check;
ALTER TABLE command_sources
  ADD CONSTRAINT command_sources_source_id_check
  CHECK (source_id ~ '^[A-Za-z0-9._:-]+$');

ALTER TABLE device_latest_states
  ADD CONSTRAINT device_latest_states_vendor_id_fkey
  FOREIGN KEY (vendor_id) REFERENCES devices(vendor_id);
ALTER TABLE command_sources
  ADD CONSTRAINT command_sources_device_vendor_id_fkey
  FOREIGN KEY (device_vendor_id) REFERENCES devices(vendor_id);
ALTER TABLE commands
  ADD CONSTRAINT commands_target_vendor_id_fkey
  FOREIGN KEY (target_vendor_id) REFERENCES devices(vendor_id);

ALTER TABLE devices
  ADD COLUMN device_type TEXT NOT NULL DEFAULT 'cns_box';
ALTER TABLE devices
  ADD CONSTRAINT devices_device_type_check
  CHECK (device_type IN ('cns_box', 'flight_controller'));

-- PX4 真实飞控允许暂未绑定学校；主控箱仍由接入层强制要求学校。
ALTER TABLE devices ALTER COLUMN school_id DROP NOT NULL;
