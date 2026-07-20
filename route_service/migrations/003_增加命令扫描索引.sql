-- 非终态命令按更新时间恢复，覆盖未来飞控状态。
CREATE INDEX commands_active_updated_idx
ON commands (updated_at, command_id)
WHERE status IN ('pending', 'dispatched', 'in_progress', 'delivery_uncertain');

-- 终态命令按完成时间分批清理。
CREATE INDEX commands_terminal_completed_idx
ON commands (completed_at, command_id)
WHERE status IN ('succeeded', 'failed', 'timeout')
  AND completed_at IS NOT NULL;
