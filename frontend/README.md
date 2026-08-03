# CNS Server / Frontend

本目录用于实现 CNS 设备管理控制前端。前端与 `backend_service` 属于同一 V1 交付范围，但保持独立构建目录和清晰协议边界。

## 技术方案

- React + Vite + TypeScript。
- Ant Design 管理后台组件。
- TanStack Query 管理 REST 快照。
- 标准原生 WebSocket 接收实时设备状态和命令结果。
- 不使用 Socket.IO、Redux、Zustand 或 ECharts。

## 页面范围

V1 使用横向桌面管理后台布局，包含：

- `/devices`：设备统计、筛选和设备列表。
- `/devices/:device_id`：设备身份、在线状态、核心遥测、完整遥测、运行时配置、飞控控制和当前命令状态。

设备列表区分“CNS 主控箱”和“PX4 真实飞控”。PX4 详情显示统一设备 ID、Remote ID 与完整 MAVLink 遥测，并禁用仅适用于主控箱私有协议的飞行控制面板。

设备列表与详情使用独立页面。遥测字段缺失时显示 `--`，不得伪造默认值。V1 不展示遥测历史曲线，也不恢复页面刷新前的命令历史。

## 协议边界

- 设备快照和命令提交使用 backend_service 的 REST API。
- 实时设备状态和命令结果使用 backend_service 的标准 WebSocket。
- 浏览器不连接 PostgreSQL、Mosquitto 或 route_service。
- 浏览器不构造设备 MQTT topic，也不直接控制设备。
- 浏览器与 backend_service 的共享 schema 位于 `backend_service/protocol`，不得把 MQTT 内部协议暴露到前端。

## 开发数据

route_service 和 backend_service 真实链路尚未就绪时，里程碑一允许使用明确标注的开发数据展示和评审页面。开发数据适配必须与真实 API 适配使用同一页面模型，并满足：

- 页面持续显示“开发数据”标识。
- 生产构建默认不能启用开发数据。
- 不把模拟命令结果描述为真实设备 ACK。
- 后续接入真实 REST/WebSocket 时不重写页面组件，只替换数据适配层。

## 当前状态

- Backend Service V1 全局设计和全局里程碑路线已经确认。
- 前端尚未开始编码。
- “里程碑一：TypeScript 全栈工程基础”设计已经逐节确认，采用独立 `frontend/dev-simulator` 通过正式 REST/WebSocket 契约驱动可点击页面。
- 页面采用明亮、专业、现代的企业级风格，主色为 `#132B88`，使用公司现有透明 Logo；设备列表和独立详情控制页的布局基线已经确认，视觉细节在可运行页面中继续优化。
- 下一步复核书面设计，再编写本里程碑详细实施计划；未经计划确认不开始编码。
- 正式 V1 设计与全局计划位于 `backend_service/docs/`；各里程碑的设计、详细计划和验证记录也统一归档在 backend_service 文档目录。
