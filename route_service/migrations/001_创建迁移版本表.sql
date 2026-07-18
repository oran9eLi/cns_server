-- 记录已经成功执行的数据库迁移，由迁移执行器保证每个版本只应用一次。
CREATE TABLE public.schema_migrations (
  version INTEGER PRIMARY KEY CHECK (version > 0),
  name TEXT NOT NULL,
  applied_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
);
