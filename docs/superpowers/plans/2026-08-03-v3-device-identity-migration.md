# V3 统一设备身份迁移 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将服务器从旧版 `vendor_id`/schema v1-v2 一次性迁移到以 Open Drone ID Basic ID 为 `device_id` 的 schema v3，并同步 REST、WebSocket、命令链路、前端与软件部 MQTT 文档。

**Architecture:** Route Service 在 MQTT 接入边界只解析 schema v3，校验 topic 中的设备 ID 与 payload 顶层 `device_id` 一致，并把字符串数组 `capabilities`、可选对象 `product`、`version` 作为注册元数据保存。数据库通过 006 迁移直接改列名和外键名，删除旧 PX4 测试记录，不做备份、不保留兼容字段；Backend、Frontend 与模拟器随后统一使用 `device_id`。PX4 realtime 与 RTT 的消息结构仍为 schema v1，只更新其 `device_id` 语义。

**Tech Stack:** C++23、nlohmann/json、PostgreSQL SQL migrations、Node.js/TypeScript、Fastify、Zod、React、Vitest、CMake/CTest、MQTT 3.1.1。

## Global Constraints

- 开发基线固定为 `origin/new2` 提交 `dc01a834d8d00ffd8a3d8ec7b9934e10b44d546a`，实施分支为 `feat/v3-schema-migration`。
- 注册与常规遥测只接受 `schema_version: 3`；schema v1、v2 直接拒绝，不提供兼容开关。
- `device_id` 唯一来源是 `OPEN_DRONE_ID_BASIC_ID.uas_id`；不再暴露或保存 `vendor_id`、`remote_id`、`uid`、`uid2`、`gateway_id`、`rpi_serial`、`endpoint`。
- PostgreSQL、C++、REST、WebSocket、命令链路、前端与文档全部使用 `device_id`，不保留 `vendor_id` 别名。
- 数据库只有测试数据，不做备份；迁移时直接删除旧 `PX4U1-`/`PX4U2-` 设备及其关联测试数据。
- PX4 realtime topic `cns_rpi/+/px4/realtime/v1` 与 RTT 协议继续使用 `schema_version: 1`，仅要求其中的 `device_id` 采用 Basic ID。
- 不新增双写、回滚迁移、旧字段映射或历史数据转换逻辑。

---

### Task 1: 固化 schema v3 MQTT 接入协议

**Files:**
- Modify: `route_service/tests/test_device_message.cpp`
- Modify: `route_service/src/core/protocol/device_message.hpp`
- Modify: `route_service/src/core/protocol/device_message.cpp`
- Modify: `route_service/tests/test_device_ingress.cpp`

**Interfaces:**
- Produces: `protocol::Registration{device_id, status, school_name, dcdw_label, device_type, capabilities, product, version}`，后三项均可缺失，其中 `capabilities` 存在时必须是字符串数组。
- Produces: `ParseRegistration(payload, topic_device_id)` 和 `ParseTelemetry(payload, topic_device_id)`，只接受 schema v3。

- [ ] **Step 1: 添加 schema v3 注册成功测试**

  测试覆盖 `device_id`、`device_type`、顶层 `school_name`/`dcdw_label`、字符串数组 `capabilities`、可选对象 `product`、`version`，并断言解析结果不再包含 `vendor_id`。另覆盖离线注册只含四个必需字段且元数据均为空。

- [ ] **Step 2: 添加旧 schema 与身份重复字段拒绝测试**

  分别输入 schema v1、v2，以及含 `identity.vendor_id`、`uid`、`uid2`、`remote_id`、`gateway_id`、`rpi_serial`、`endpoint` 的 v3 payload；预期 `ParseRegistration`/`ParseTelemetry` 返回错误。另测试 payload `device_id` 与 topic 不一致时拒绝。

- [ ] **Step 3: 运行协议测试并确认失败**

  Run: `cmake --build route_service/build -j2 --target test_device_message test_device_ingress && ctest --test-dir route_service/build -R 'device_message|device_ingress' --output-on-failure`

  Expected: 新增 v3 断言失败，证明旧解析器仍接受 v1/v2 或未读取新字段。

- [ ] **Step 4: 最小化实现 schema v3 解析**

  将 `Registration::vendor_id` 改为 `device_id`；只允许整数 `3`。注册读取顶层归属字段、可选字符串数组 `capabilities` 和可选对象 `product`/`version`；遥测要求顶层 `device_id`、`device_type`、`sent_at`、`telemetry`、`drone_id.basic_id`，且 `basic_id` 中不允许重复 `uas_id`。保留完整遥测 JSON 供后续入库，但不从已废弃 identity 字段提取信息。

- [ ] **Step 5: 运行协议测试并确认通过**

  Run: `cmake --build route_service/build -j2 --target test_device_message test_device_ingress && ctest --test-dir route_service/build -R 'device_message|device_ingress' --output-on-failure`

  Expected: 相关测试全部通过。

- [ ] **Step 6: 提交协议边界改动**

  ```bash
  git add route_service/src/core/protocol route_service/tests/test_device_message.cpp route_service/tests/test_device_ingress.cpp
  git commit -m "feat(route): require v3 device messages"
  ```

### Task 2: 数据库与 Route Service 全量改为 device_id

**Files:**
- Create: `route_service/migrations/006_统一设备标识为device_id.sql`
- Modify: `route_service/tests/test_schema_sql.cpp`
- Modify: `route_service/tests/test_migration.cpp`
- Modify: `route_service/tests/test_migration_plan.cpp`
- Modify: `route_service/src/core/device/device_registry.hpp`
- Modify: `route_service/src/core/device/device_registry.cpp`
- Modify: `route_service/src/core/mqtt_topic/device_topic.hpp`
- Modify: `route_service/src/core/mqtt_topic/device_topic.cpp`
- Modify: `route_service/src/core/state_event/state_event.hpp`
- Modify: `route_service/src/core/state_event/state_event.cpp`
- Modify: `route_service/src/core/command/command_types.hpp`
- Modify: `route_service/src/core/command/command_state.hpp`
- Modify: `route_service/src/core/command/command_state.cpp`
- Modify: `route_service/src/core/command/command_router.hpp`
- Modify: `route_service/src/core/command/command_router.cpp`
- Modify: `route_service/src/core/command/source_request.cpp`
- Modify: `route_service/src/core/runtime/device_service.hpp`
- Modify: `route_service/src/core/runtime/device_service.cpp`
- Modify: `route_service/src/core/runtime/command_service.hpp`
- Modify: `route_service/src/core/runtime/command_service.cpp`
- Modify: `route_service/src/core/runtime/postgres_worker.cpp`
- Modify: `route_service/src/core/persistence/dirty_state.cpp`
- Modify: `route_service/src/adapters/postgres/postgres_store.hpp`
- Modify: `route_service/src/adapters/postgres/postgres_store.cpp`
- Modify: `route_service/src/service_environment.cpp`
- Modify: affected files under `route_service/tests/`

**Interfaces:**
- Consumes: Task 1 的 `Registration::device_id`。
- Produces: `DeviceRecord::device_id`，以及数据库列 `devices.device_id`、`device_latest_states.device_id`、`command_sources.device_id`、`commands.target_device_id`。
- Produces: Route Service 目录/状态消息只含 `device_id`，命令 target 只含 `device_id`。

- [ ] **Step 1: 添加 006 迁移结构测试**

  断言迁移先删除关联 `PX4U1-%`/`PX4U2-%` 测试行，再将四个旧列改名、重建指向 `devices(device_id)` 的外键，并给 `devices` 增加可空的 `capabilities`、`product`、`version` JSONB 列；断言 SQL 不包含备份表或兼容列。

- [ ] **Step 2: 运行迁移测试并确认失败**

  Run: `cmake --build route_service/build -j2 --target test_schema_sql test_migration test_migration_plan && ctest --test-dir route_service/build -R 'schema_sql|migration' --output-on-failure`

  Expected: 因 006 尚不存在而失败。

- [ ] **Step 3: 编写直接迁移 SQL**

  删除关联命令、来源、状态和旧 PX4 设备，随后执行 `RENAME COLUMN` 与约束重建；新元数据列使用可空 `JSONB`，使“从未上报”和“上报空数组/空对象”保持不同语义。迁移保持单事务执行，不创建备份。

- [ ] **Step 4: 将 Route Service 标识命名机械改为 device_id**

  覆盖 registry、topic、state event、command、runtime 与 PostgreSQL adapter；同时更新日志、错误信息、排序键和 SQL 参数名。删除 `IsValidVendorId`，统一使用 `IsValidDeviceId`。

- [ ] **Step 5: 保存和发布 v3 注册元数据**

  `ProvisionDevice`/注册更新仅在对应字段存在时写入 `capabilities`、`product`、`version`，离线注册不得清空旧元数据；目录与设备状态事件按 v3 公共模型输出这些字段，不再输出 `vendor_id`。

- [ ] **Step 6: 更新 Route Service 单元测试并运行全套测试**

  Run: `cmake --build route_service/build -j2 && ctest --test-dir route_service/build --output-on-failure`

  Expected: 34 个 CTest 全部通过，源码与测试中 `rg -n 'vendor_id|target_vendor_id|device_vendor_id' route_service/src route_service/tests` 无结果。

- [ ] **Step 7: 提交数据库和 Route Service 改动**

  ```bash
  git add route_service/migrations route_service/src route_service/tests
  git commit -m "refactor(route): unify persistence on device id"
  ```

### Task 3: Backend 公共协议、REST、WebSocket 与命令链路改名

**Files:**
- Modify: `backend_service/protocol/src/common.ts`
- Modify: `backend_service/protocol/src/devices.ts`
- Modify: `backend_service/protocol/src/commands.ts`
- Modify: `backend_service/protocol/src/rest.ts`
- Modify: `backend_service/src/devices/deviceStore.ts`
- Modify: `backend_service/src/devices/postgresDeviceStore.ts`
- Modify: `backend_service/src/devices/deviceRoutes.ts`
- Modify: `backend_service/src/devices/seedDevices.ts`
- Modify: `backend_service/src/route/routeProtocol.ts`
- Modify: `backend_service/src/route/routeServiceGateway.ts`
- Modify: `backend_service/src/commands/commandTracker.ts`
- Modify: affected files under `backend_service/test/`

**Interfaces:**
- Consumes: Task 2 的 Route MQTT 目录、状态、命令协议和 PostgreSQL 列名。
- Produces: `/api/devices/:device_id`、`/api/devices/:device_id/commands`、`/api/devices/:device_id/px4-latency-probes`，响应与 WebSocket 事件只含 `device_id`。

- [ ] **Step 1: 更新协议测试以只接受 device_id**

  为设备摘要、命令接受响应、`command.updated`、Route 目录/状态和命令 target 添加 strict schema 断言：`device_id` 成功，任何 `vendor_id` 都失败。

- [ ] **Step 2: 运行 Backend 测试并确认失败**

  Run: `npm --prefix backend_service test`

  Expected: 旧协议仍要求或输出 `vendor_id`，新增断言失败。

- [ ] **Step 3: 更新 Zod schema 与 REST 路径参数**

  删除 `VendorIdSchema`；设备、命令响应、WebSocket 事件统一为 `DeviceIdSchema`。将 REST 路径参数和 handler 变量改为 `device_id`/`deviceId`，不改变 URL 的实际层级。

- [ ] **Step 4: 更新 PostgreSQL store 与 Route gateway**

  SQL 查询使用 Task 2 新列；Route 消息 strict schema 删除 `vendor_id` 并加入 v3 注册元数据；命令 payload 使用 `target: { device_id }`，命令追踪结构统一命名为 `deviceId`。

- [ ] **Step 5: 运行 Backend 验证**

  Run: `npm --prefix backend_service test && npm --prefix backend_service run typecheck && npm --prefix backend_service run build`

  Expected: 21 个 Vitest 全部通过，类型检查与构建成功；`rg -n 'vendor_id|VendorId|vendorId' backend_service/src backend_service/test backend_service/protocol/src` 仅允许命中“旧字段必须被拒绝”的测试断言。

- [ ] **Step 6: 提交 Backend 改动**

  ```bash
  git add backend_service
  git commit -m "refactor(backend): expose device id only"
  ```

### Task 4: Frontend 与开发模拟器改用 device_id

**Files:**
- Modify: `frontend/src/App.tsx`
- Modify: `frontend/src/realtime/useRealtime.ts`
- Modify: `frontend/src/utils/telemetryFrame.ts`
- Modify: `frontend/src/utils/telemetryFrame.test.ts`
- Modify: `frontend/dev-simulator/src/fixtures/devices.ts`
- Modify: `frontend/dev-simulator/src/server.ts`
- Modify: affected frontend tests under `frontend/src/`

**Interfaces:**
- Consumes: Task 3 的 `DeviceSummary.device_id`、`DeviceStateEvent.device_id` 和 REST `:device_id`。
- Produces: 列表 key、详情路由、搜索、实时状态合并与命令事件过滤全部按 `device_id`。

- [ ] **Step 1: 将前端测试夹具和断言改为 device_id**

  删除重复 `vendor_id`，增加列表跳转、WebSocket 合并和 telemetry label 不再识别 `identity.vendor_id` 的断言。

- [ ] **Step 2: 运行 Frontend 测试并确认失败**

  Run: `npm --prefix frontend test`

  Expected: 旧组件和模拟器仍读取 `vendor_id`，新增断言失败。

- [ ] **Step 3: 更新 UI、状态合并与模拟器**

  列名显示“设备 ID”，路由参数改为 `deviceId`，搜索提示移除 vendor 表述；模拟器的 REST、状态事件、命令事件与 v3 慢速遥测夹具只生成 `device_id`。PX4 realtime/RTT fixture 仍保持 schema v1。

- [ ] **Step 4: 运行 Frontend 验证**

  Run: `npm --prefix frontend test && npm --prefix frontend run typecheck && npm --prefix frontend run build`

  Expected: 21 个 Vitest 全部通过，类型检查与构建成功；`rg -n 'vendor_id|VendorId|vendorId' frontend/src frontend/dev-simulator/src` 仅允许命中“旧字段不得展示”的测试断言。

- [ ] **Step 5: 提交 Frontend 改动**

  ```bash
  git add frontend
  git commit -m "refactor(frontend): use unified device id"
  ```

### Task 5: 更新软件部 MQTT 接入文档并完成联调验收

**Files:**
- Modify: `route_service/docs/2026-07-28-软件部公网MQTT只读数据接入说明.md`
- Modify: `README.md`（仅当其中仍声明旧 schema 或 `vendor_id`）
- Modify: deployment/config documentation found by `rg`（仅限协议字段相关段落）

**Interfaces:**
- Consumes: Tasks 1-4 的最终公共消息格式。
- Produces: 软件部可直接据此订阅主控箱和 PX4 数据的唯一接入说明。

- [ ] **Step 1: 将文档示例升级为 schema v3**

  目录、状态和慢速遥测示例只使用 Basic ID `device_id`，删除 `vendor_id` 兼容字段和 `PX4U1-`/`PX4U2-` 规则；说明 `device_type` 可用于展示分类，但不得描述为“仅处理主控箱”。

- [ ] **Step 2: 完整说明 PX4 帧**

  给出 v3 慢速遥测的 `drone_id.basic_id` 与 `telemetry` 示例；保留 realtime topic、QoS 0、非 retained、约 20 Hz、schema v1 和 Basic ID 语义。明确接入方可按需查看 PX4，不要求消费所有字段。

- [ ] **Step 3: 执行旧字段与版本扫描**

  Run: `rg -n 'vendor_id|target_vendor_id|device_vendor_id|PX4U1-|PX4U2-|schema_version[^\n]*(1 或 2|1 或 2)' route_service/src route_service/tests backend_service/src backend_service/test backend_service/protocol/src frontend/src frontend/dev-simulator/src route_service/docs README.md`

  Expected: 仅历史迁移 001-005 或明确说明“旧版被拒绝”的文档段落允许命中。

- [ ] **Step 4: 执行最终全量验证**

  Run: `cmake --build route_service/build -j2 && ctest --test-dir route_service/build --output-on-failure && npm --prefix backend_service test && npm --prefix backend_service run typecheck && npm --prefix backend_service run build && npm --prefix frontend test && npm --prefix frontend run typecheck && npm --prefix frontend run build`

  Expected: Route Service 34 个测试、Backend 21 个测试、Frontend 21 个测试全部通过，协议包、Backend、Frontend 与开发模拟器构建成功。

- [ ] **Step 5: 提交文档与验收结果**

  ```bash
  git add route_service/docs README.md docs/superpowers/plans/2026-08-03-v3-device-identity-migration.md
  git commit -m "docs: publish v3 device integration guidance"
  ```

## 部署顺序

1. 停止 Route Service 与 Backend，避免迁移窗口内继续接收旧消息。
2. 部署含 006 migration 的 Route Service；启动时完成测试库直接迁移。
3. 部署 Backend 与 Frontend。
4. 启动已实现 v3 的 RPi，确认注册、慢速遥测、PX4 realtime 和 RTT。
5. 使用全新 MQTT 客户端订阅目录与状态 retained topic，核对主控箱和 PX4 均以 Basic ID 出现。
6. 抽样发送 schema v1/v2 注册和慢速遥测，确认服务器记录拒绝日志且数据库不产生新状态。

## 回滚边界

本次不提供数据库回滚脚本。若测试环境部署失败，回退应用版本后重建测试库并重新录入测试设备；不得让旧服务连接已完成 006 的数据库。
