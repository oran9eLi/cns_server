# CNS Server / Backend Service

`backend_service` 是浏览器和 `route_service` 之间的 Web 服务，只负责 REST、WebSocket、PostgreSQL 只读查询以及 MQTT 协议适配。

## 服务边界

- 从 `schools`、`devices`、`device_latest_states` 读取设备快照。
- 订阅 `route_service` 发布的规范化设备状态事件。
- 使用固定来源 `web-console` 向 `route_service` 发布配置和飞控请求。
- 按 MQTT `request_id` 将 ACK 转发到原浏览器 WebSocket 会话。
- 不修改 route_service 代码，不写核心数据库表，不直接向设备 `/config/set` 或 `/control/set` topic 发布。
- REST/WebSocket 只输出统一 `device_id` 和 `device_type`，不提供旧标识兼容别名；PX4 的学校可以为空。
- PX4 展示遥测与 Remote ID，但不接受主控箱私有 control 命令。
- 实时遥测传输不区分设备类型，不包含 PX4 专属 Topic、RTT 探测或处理分支。

## MQTT Topic

面向软件部的 Route Service 全量设备目录为：

```text
{namespace}/events/devices/directory
```

该目录包含全部已入库设备和在线状态。当前 backend_service 仍通过 PostgreSQL 建立 REST 冷启动快照，因此运行时只订阅下列单设备状态和命令 ACK，不重复维护全量目录缓存。

订阅：

```text
{namespace}/events/devices/+/state
{namespace}/sources/web-console/config/ack
{namespace}/sources/web-console/control/ack
```

浏览器进入设备详情页后，通过 WebSocket 发送
`telemetry.subscribe(device_id)`。只有某设备至少有一个查看会话时，服务才精确订阅：

```text
{namespace}/{device_id}/telemetry/realtime/v1
```

实时帧使用 schema v1、QoS 0、非 retained，只转发给关注该设备的会话，不写数据库。
同一设备的多个页面共享一个 MQTT 订阅，最后一个页面离开后退订；MQTT 重连只恢复仍有页面兴趣的设备。

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

真实联调时应确认 `/api/health` 中 `database` 和 `mqtt` 都为 `ready`，然后验证设备状态事件、通用实时遥测和命令 ACK 能通过 `/ws` 到达浏览器。

## 硬件服务器部署

2026-07-28 已在 `hardware@192.168.11.3` 完成生产构建和 systemd 部署：

- 配置文件：`/etc/cns/backend_service.json`，权限 `0600`，不进入 Git。
- 服务单元：`cns-backend-service.service`，以 `hardware` 用户运行并设置为开机自启。
- HTTP 监听：`0.0.0.0:3000`，由 Nginx 在 `80` 端口代理 `/api/` 和 `/ws`。
- PostgreSQL：使用 `cns_backend_read`，仅对 `schools`、`devices`、`device_latest_states` 具有 `SELECT` 权限。
- MQTT：连接硬件服务器本机 `127.0.0.1:1883`。
- 局域网入口：`http://192.168.11.3/`。

部署验收时 `/api/health` 的 `database` 和 `mqtt` 均为 `ready`。2026-07-28 已随 Route Service 全量设备目录协议同步重建并重启，能够接受不含内部 `school_id` 的目录和单设备状态消息。树莓派已通过公网 FRP 接入硬件服务器，真实遥测已到达 Broker、数据库和后端设备详情接口；浏览器 WebSocket 展示和命令 ACK 闭环仍待补验。
