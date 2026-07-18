# CNS Server / Backend Service

本目录用于后续实现 CNS 设备管理前端对应的服务器端接口。

## 职责边界

`backend_service` 位于浏览器与 `route_service` 之间，预期负责：

- 向前端提供设备列表、设备详情和实时状态数据。
- 接收前端控制请求并生成来源侧 `request_id`。
- 以固定、受控的命令来源身份向 `route_service` 提交请求。
- 按 `request_id` 把路由或设备执行结果返回正确的前端会话。
- 在后续阶段承载登录、用户权限、学校数据隔离和操作审计。

## 禁止事项

- 不直接向 `{namespace}/{vendor_id}/config/set` 或 `control/set` 发布消息。
- 不绕过 `route_service` 的来源登记、学校权限、同校限制和幂等检查。
- 不拥有 `schools`、`devices` 和命令路由核心表的写权限。
- 不在本阶段提前锁定前端、后端框架、通信接口或部署技术。

## 与 route_service 的关系

对 `route_service` 而言，整个 `backend_service` 是一个固定命令来源，例如 `web-console`。多个浏览器会话的区分、前端请求状态和会话级结果分发由 `backend_service` 自己维护，不扩散到设备端协议。

设备注册、遥测身份补全、在线状态维护、目标寻址、服务器级权限、命令持久化和 ACK 回程均由 `route_service` 负责。

## 当前状态

- 尚未进入设计与实现阶段。
- 技术选型和接口细节留待 `route_service` 完成后单独讨论。
- 现有桌面文档《CNS设备管理前端_实施计划.md》仅作为后续需求输入，其中涉及的技术栈和接口不视为已经确认。
