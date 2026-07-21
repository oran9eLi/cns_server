# Backend Service Protocol

本目录定义浏览器与 `backend_service` 之间的正式 REST/WebSocket 契约。

协议包由 `backend_service` 拥有，供以下模块复用：

- `backend_service`：校验 HTTP 请求、HTTP 响应和 WebSocket 事件。
- `frontend`：生成 TypeScript 类型，并在接收 REST/WebSocket 数据时校验。
- `frontend/dev-simulator`：按正式契约提供开发数据。

本包不得依赖 Fastify、React、MQTT.js、PostgreSQL 或开发模拟器，也不得暴露 MQTT topic、数据库表结构或设备端内部协议。

## 端点

```text
GET  /api/health
GET  /api/devices
GET  /api/devices/:vendor_id
POST /api/devices/:vendor_id/commands
WS   /ws
```

所有正式响应和 WebSocket 事件使用 `schema_version=1`。时间字段使用 UTC RFC 3339 毫秒格式。

## REST 概览

### `GET /api/health`

返回服务状态、服务器时间和依赖状态。里程碑一中真实依赖尚未接入时，可以返回 `not_configured`。

### `GET /api/devices`

查询参数：

- `keyword`：可搜索 `vendor_id`、角色号和学校。
- `school_name`：按学校筛选。
- `status`：`online` 或 `offline`。

响应使用 `{ "items": [...] }` 包装，为后续分页保留兼容空间。

### `GET /api/devices/:vendor_id`

返回设备身份、当前状态、完整 `latest_telemetry` 和降级标识。遥测字段缺失时由前端显示 `--`，协议层不伪造默认值。

### `POST /api/devices/:vendor_id/commands`

命令请求必须携带当前 WebSocket 会话的 `session_id` 和浏览器生成的 `client_request_id`。浏览器重试同一次操作时必须复用原 `client_request_id`。

配置命令：

```json
{
  "session_id": "session_01HX...",
  "client_request_id": "00000000-0000-4000-8000-000000000001",
  "type": "config",
  "parameters": {
    "telemetry_publish_interval_ms": 2000
  }
}
```

飞控 PWM 命令：

```json
{
  "session_id": "session_01HX...",
  "client_request_id": "00000000-0000-4000-8000-000000000002",
  "type": "control",
  "command": "set_motor_pwm",
  "parameters": {
    "motor_pwm": [1000, 1000, 1000, 1000]
  }
}
```

飞控无参数命令：

```json
{
  "session_id": "session_01HX...",
  "client_request_id": "00000000-0000-4000-8000-000000000003",
  "type": "control",
  "command": "takeoff",
  "parameters": {}
}
```

`land` 和 `emergency_stop` 使用同样的空参数结构。

## WebSocket 事件

服务端建立连接后先发送：

```json
{
  "type": "session.ready",
  "schema_version": 1,
  "session_id": "session_01HX...",
  "server_time": "2026-07-19T00:00:00.000Z"
}
```

设备状态广播给全部连接：

```json
{
  "type": "device.state",
  "schema_version": 1,
  "vendor_id": "CNS00000000000000001",
  "status": "online",
  "last_seen_at": "2026-07-19T00:00:00.000Z",
  "telemetry_received_at": "2026-07-19T00:00:00.000Z",
  "latest_telemetry": {},
  "degraded": false
}
```

命令结果只发送给原提交会话：

```json
{
  "type": "command.updated",
  "schema_version": 1,
  "client_request_id": "00000000-0000-4000-8000-000000000003",
  "vendor_id": "CNS00000000000000001",
  "command_type": "control",
  "command": "takeoff",
  "status": "succeeded",
  "business_status": "accepted",
  "error": null,
  "updated_at": "2026-07-19T00:00:00.000Z"
}
```

## 当前边界

- 运行时配置字段中，当前文档只明确了 `telemetry_publish_interval_ms`。其余已确认字段进入设计后，应在 `RuntimeConfigParametersSchema` 中补成显式字段。
- `latest_telemetry` 保持 JSON 对象，不在协议包中拆成固定遥测字段，避免前端伪造缺失值。
- `business_status` 保留 route_service 或设备 ACK 的业务状态字符串，统一命令状态由 `status` 表达。
