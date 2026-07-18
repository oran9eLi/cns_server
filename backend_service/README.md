# CNS Server / Backend Service

本目录用于实现 CNS 设备管理前端对应的服务器端接口。

## 职责边界

`backend_service` 位于浏览器与 `route_service` 之间，预期负责：

- 向前端提供设备列表、设备详情和实时状态数据。
- 启动时从 PostgreSQL 读取设备快照，随后订阅 `route_service` 输出的规范化 MQTT 状态事件；不直接把设备原始 telemetry 作为权威数据源。
- 接收前端控制请求，以浏览器生成的 `client_request_id` 作为来源侧 `request_id`。
- 以固定、受控的命令来源身份向 `route_service` 提交请求。
- 按 `request_id` 把路由或设备执行结果返回正确的前端会话。
- 在后续阶段承载登录、用户权限、学校数据隔离和操作审计。

## 禁止事项

- 不直接向 `{namespace}/{vendor_id}/config/set` 或 `control/set` 发布消息。
- 不绕过 `route_service` 的来源登记、学校权限、同校限制和幂等检查。
- 不拥有 `schools`、`devices` 和命令路由核心表的写权限。
- 不把浏览器会话扩散为 route_service 或设备端身份。

## 与 route_service 的关系

对 `route_service` 而言，整个 `backend_service` 是一个固定命令来源，例如 `web-console`。多个浏览器会话的区分、前端请求状态和会话级结果分发由 `backend_service` 自己维护，不扩散到设备端协议。

设备注册、遥测身份补全、在线状态维护、目标寻址、服务器级权限、命令持久化和 ACK 回程均由 `route_service` 负责。

`route_service` 把 `web-console` 视为可访问全部学校的固定 `host_app` 来源。未来“账号只能访问本校或可以访问全部学校”的权限必须由本服务在认证后执行，不能依赖前端页面过滤，也不扩展为 `route_service` 的账号模型。

## 当前状态

- V1 设计已经逐节确认，尚未开始实现。
- 后端采用 Node.js、TypeScript、Fastify、MQTT.js、`pg` 和 Zod。
- 前端作为仓库顶层独立 `frontend/` 子项目，采用 React、Vite、TypeScript、Ant Design 和 TanStack Query。
- 查询与命令提交使用 REST，实时设备状态和命令结果使用标准原生 WebSocket，不使用 Socket.IO。
- V1 使用 Nginx Basic Auth，并要求公网入口运行在 HTTPS 之上；backend_service 由 systemd 管理。
- V1 覆盖全部运行时配置与飞控命令，但不包含应用账号、命令历史恢复、遥测历史、多实例高可用和移动端专项适配。
- 正式设计见 `docs/2026-07-19-后端服务V1设计.md`。
- 全局里程碑路线见 `docs/2026-07-19-后端服务V1实施计划.md`；每个里程碑开始前另写设计和详细实施计划。
- 现有桌面文档《CNS设备管理前端_实施计划.md》仅作为需求输入，其中与正式设计不一致的技术栈和接口不再作为实现依据。
