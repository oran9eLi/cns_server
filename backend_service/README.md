# CNS Server / Backend Service

`backend_service` 是浏览器和 `route_service` 之间的 Web 服务，只负责 REST、WebSocket、PostgreSQL 只读查询以及 MQTT 协议适配。

## 服务边界

- 从 `schools`、`devices`、`device_latest_states` 读取设备快照。
- 订阅 `route_service` 发布的规范化设备状态事件。
- 使用固定来源 `web-console` 向 `route_service` 发布配置和飞控请求。
- 按 MQTT `request_id` 将 ACK 转发到原浏览器 WebSocket 会话。
- 不修改 route_service 代码，不写核心数据库表，不直接向设备 `/config/set` 或 `/control/set` topic 发布。

## MQTT Topic

订阅：

```text
{namespace}/events/devices/+/state
{namespace}/sources/web-console/config/ack
{namespace}/sources/web-console/control/ack
```

发布（QoS 2、非 retained）：

```text
{namespace}/sources/web-console/config/request
{namespace}/sources/web-console/control/request
```

## 配置

复制 `config/backend_service.example.json` 到仓库外的本机配置文件，替换 PostgreSQL 和 MQTT 连接信息。数据库账号只授予以下表的 `SELECT` 权限：

```text
schools
devices
device_latest_states
```

PowerShell 启动：

```powershell
$env:CNS_BACKEND_CONFIG="C:\path\to\backend-service.local.json"
npm.cmd run dev:backend
```

前后端同时启动：

```powershell
$env:CNS_BACKEND_CONFIG="C:\path\to\backend-service.local.json"
npm.cmd run dev:integration
```

页面地址：`http://127.0.0.1:5173/devices`。

未配置 MQTT 时，设备内存种子数据仍可用于查看页面，但命令提交会被禁用并返回 `mqtt_unavailable`。需要完整模拟交互时使用 `npm.cmd run dev:frontend`，它会启动独立前端模拟器。

## 验证

```powershell
npm.cmd run typecheck
npm.cmd run test
npm.cmd run build
```

真实联调时应确认 `/api/health` 中 `database` 和 `mqtt` 都为 `ready`，然后验证设备状态事件和命令 ACK 能通过 `/ws` 到达浏览器。

## 硬件服务器部署

2026-07-28 已在 `hardware@192.168.11.3` 完成生产构建和 systemd 部署：

- 配置文件：`/etc/cns/backend_service.json`，权限 `0600`，不进入 Git。
- 服务单元：`cns-backend-service.service`，以 `hardware` 用户运行并设置为开机自启。
- HTTP 监听：`0.0.0.0:3000`，由 Nginx 在 `80` 端口代理 `/api/` 和 `/ws`。
- PostgreSQL：使用 `cns_backend_read`，仅对 `schools`、`devices`、`device_latest_states` 具有 `SELECT` 权限。
- MQTT：连接硬件服务器本机 `127.0.0.1:1883`。
- 局域网入口：`http://192.168.11.3/`。

部署验收时 `/api/health` 的 `database` 和 `mqtt` 均为 `ready`。树莓派当时离线，因此真实遥测增量、WebSocket 推送和命令 ACK 闭环仍待设备切换 Broker 后补验。
