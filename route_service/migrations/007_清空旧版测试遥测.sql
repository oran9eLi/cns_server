-- 当前数据库只有测试数据；v3 迁移后不再向外重放旧版慢速遥测。
UPDATE device_latest_states
SET latest_telemetry = NULL,
    telemetry_received_at = NULL
WHERE latest_telemetry IS NOT NULL
  AND (
    NOT (latest_telemetry ? 'schema_version')
    OR latest_telemetry->>'schema_version' <> '3'
  );
