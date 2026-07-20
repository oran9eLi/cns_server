# 任务 4 实施报告

## 状态

- 已新增 `DirtyState`，每个 vendor 只保存一个最新期望状态。
- metadata、status、telemetry 分字段跟踪 dirty 与 in-flight revision。
- Immediate 会合并并携带同设备待写 telemetry；TelemetryBatch 按间隔取出。
- `Complete` 不清除更晚 revision，`Restore` 与失败期间的新值按字段合并。

## TDD 证据

- RED：重新配置 `route_service/build-m2` 后，真实 `test_dirty_state` 目标成功构建；空实现运行 6 个测试用例全部因状态合并行为缺失而失败。
- GREEN：实现后 `ctest --test-dir route_service/build-m2 -R '^dirty_state$' --output-on-failure` 通过。

## 验证

- `cmake --build route_service/build-m2 -j2`：通过。
- `ctest --test-dir route_service/build-m2 --output-on-failure`：20/20 通过。
- `git diff --check`：通过。

## 自审

- 未保存 telemetry 帧队列，容器维度为 vendor。
- 新 record 仅在 revision 不旧于当前值时替换，失败恢复不会覆盖期间到达的新 record。
- 未修改 `cns_rpi/`、迁移或任务列明范围外的产品文件。

## 顾虑

- 当前类按调用方串行调度同一 vendor 的持久化结果设计；接口本身不携带单次请求 ID，不适合对同 vendor 并发完成多个相同 revision 的写请求。
