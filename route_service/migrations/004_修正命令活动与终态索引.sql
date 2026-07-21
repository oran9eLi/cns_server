-- 飞控活动命令在服务恢复时收敛为终态，因此不再属于活动扫描范围。
DROP INDEX IF EXISTS commands_active_updated_idx;
CREATE INDEX commands_active_updated_idx ON commands (updated_at, command_id)
WHERE status IN ('pending', 'dispatched', 'in_progress');

-- delivery_uncertain 与其他终态遵循相同的保留和批量清理策略。
DROP INDEX IF EXISTS commands_terminal_completed_idx;
CREATE INDEX commands_terminal_completed_idx ON commands (completed_at, command_id)
WHERE status IN ('succeeded', 'failed', 'timeout', 'delivery_uncertain')
  AND completed_at IS NOT NULL;
