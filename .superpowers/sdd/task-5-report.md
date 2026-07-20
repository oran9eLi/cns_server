# 任务 5 实施报告

## 结果

- 为 `PostgresStore` 增加设备全量加载、单事务建档、按脏字段写入状态及连接打开状态接口。
- 加载 SQL 连接 `devices`、`schools`、`device_latest_states`，数据库时间通过 Unix 秒与 `system_clock::time_point` 双向转换。
- 建档在单个 `pqxx::work` 中写入学校、设备、最新状态和设备命令来源；已有来源冲突使用 `DO NOTHING`，不会重新启用禁用来源。
- JSONB 仅通过参数传递，加载与写入均拒绝非 object 遥测；数据库异常返回固定安全文本，不包含连接密码或 payload。
- 新增始终运行的静态 SQL 契约测试；真实数据库加载子用例仅在显式设置 `CNS_TEST_POSTGRES` 及配套连接环境变量时启用。

## TDD 证据

- RED：`test_postgres_device_store` 首次构建因 `ProvisionRequest`、`LoadDevices`、`ProvisionDevice`、`WriteDeviceState`、`IsOpen` 缺失而失败，退出码 2。
- GREEN：实现后指定目标构建成功，`postgres_store` 与 `postgres_device_store` 2/2 通过。

## 验证

- `cmake --build route_service/build-m2 -j2`：成功。
- `ctest --test-dir route_service/build-m2 --output-on-failure`：21/21 通过。
- `git diff --check`：无错误。
- `git diff -- route_service/migrations`：无输出，已发布迁移未修改。

## 顾虑

- 当前环境未提供显式真实 PostgreSQL 测试变量，因此真实数据库子用例本次未执行；默认静态契约和全部非数据库测试均已执行。
