# CNS Server

CNS（通信、导航、监视）实训箱的服务器端工程，与树莓派端 `cns_rpi` 和 STM32 固件共同组成完整数据链路。

本工程负责接收设备注册与遥测消息、维护权威设备数据库、执行命令寻址和路由，并在后续为设备管理前端提供服务端接口。服务器不解析 UART 或 MAVLink 二进制帧；这些工作由树莓派端完成。

`cns_server` 采用单一 Git 仓库管理，`route_service` 与 `backend_service` 是同一仓库内的两个子项目，不分别建立嵌套 Git 仓库。分支、提交、版本和顶层文档由仓库统一维护，各子项目可以保留自己的构建、测试和部署入口。

## 子项目

```text
cns_server/
├── route_service/       # 设备数据库维护、权限校验、命令路由与 ACK 回程
├── backend_service/     # 面向设备管理前端的服务端接口
└── frontend/            # React 设备管理控制前端
```

### route_service

当前优先实施的核心服务。它以 MQTT 客户端身份连接 Mosquitto，消费设备注册、遥测和命令 ACK，以 PostgreSQL 作为权威持久化存储，并向目标设备发布规范化命令。

当前状态：里程碑一至三已完成；里程碑四“飞控命令路由”已完成代码和本机自动化验收，独立 PostgreSQL/Mosquitto 联调及真实 RPi 飞控验证尚未执行。现场服务器部署与综合验收仍属于里程碑五。

详细边界见 `route_service/README.md`。具体架构、表结构和实现步骤必须先经过设计文档与实施计划确认。

### backend_service

面向设备管理前端的服务。它只读 `route_service` 维护的设备数据，订阅规范化实时状态事件，并作为受控命令来源向路由服务提交请求，不得绕过路由服务直接向设备命令 topic 发布。

V1 已确认使用 TypeScript、Fastify、React、Ant Design、REST 和标准原生 WebSocket。正式设计见 `backend_service/docs/2026-07-19-后端服务V1设计.md`。
全局实施路线见 `backend_service/docs/2026-07-19-后端服务V1实施计划.md`。

### frontend

React 设备管理控制页面，与 backend_service 共用浏览器侧 REST/WebSocket 协议。当前先在里程碑一完成可查看的页面骨架和开发数据适配，待后续里程碑接入真实设备快照、实时状态与命令链路。详细边界见 `frontend/README.md`。

## 开发流程

服务器端沿用 `cns_rpi` 的协作流程：

1. 开始功能前先阅读仓库规范和现有协议。
2. 设计文档和详细实施计划直接在 `main` 编写、逐步确认并提交；文档阶段不建立功能工作树。
3. 实施计划确认后，从最新 `main` 建立隔离工作树和功能分支，按计划开发和测试，完成后再合入 `main`。
4. 提交信息使用 `<type>: <简短中文说明>`，其中 `type` 使用英文，例如 `feat: 增加设备注册处理`。
5. 文档、代码注释、运行日志和回答使用中文；协议字段、类型名等技术标识符保留英文。
6. 不提交真实密码、证书私钥、数据库数据目录或现场配置，只提交示例配置。
7. 一旦技术选型、架构边界、协议、数据结构、部署方式或验收标准得到新确认，必须在同一工作阶段主动同步所有受影响文档，不等待功能完成，也不依赖用户再次提醒。代码、示例配置和测试若受影响，也必须在实施时保持一致；不得让已经确认的事实只停留在对话中。

后续应在各子项目自己的 `docs/` 中补充协作规范、设计文档、实施计划和验证记录，不能长期依赖另一个仓库中的说明。全局设计与全局实施计划直接放在子项目 `docs/` 根目录；单个里程碑的设计、详细计划和验收记录放在对应 `docs/superpowers/` 或 `docs/change_history/`。设计和计划直接在 `main` 编写并确认，验收记录随实现工作树合入。文件名统一使用“`YYYY-MM-DD-中文主题.md`”。

## 构建与验证原则

- 日常代码编辑在开发机完成。
- 不依赖 PostgreSQL、Mosquitto 或现场网络的单元测试可在开发机运行。
- 每个集成阶段必须在硬件部服务器拉取待验证提交后执行构建、数据库迁移和集成测试。
- PostgreSQL 数据库最终保留在服务器；开发机上的临时数据库不能作为最终验收依据。
- 服务器验证不得清空或覆盖已有数据库。迁移和部署脚本必须幂等，并在修改数据库结构前保留可恢复路径。
- 无法在服务器完成的验证项必须明确记录，不能写成已通过。

## 当前优先级

1. 设计并实现 `route_service`。
2. 完成设备注册、最新遥测状态、实时状态事件、配置与飞控命令路由及 ACK 回程；V1 不保存逐帧遥测历史。
3. 完成服务器部署和 PostgreSQL/Mosquitto 集成验证。
4. 按已确认设计实施 `backend_service` 和设备管理前端；实施必须基于包含最新 route_service 成果的 `main` 新建隔离工作树。

## 已确认的 Route Service V1 边界

- 使用 C++23 单体 systemd 服务、libmosquitto、libpqxx、nlohmann/json 和 doctest。
- PostgreSQL 将稳定设备元数据与高频最新状态分表，遥测以 JSONB 保存最新快照并默认每 5 秒合并写入。
- route_service 通过 MQTT 发布 QoS 0、非 retained 的规范化实时状态事件，未来 backend_service 不直接把设备原始 telemetry 作为权威数据源。
- 配置命令请求、设备下发和两侧 ACK 使用 QoS 2、非 retained；来源幂等键为 `(source_id, request_id)`，设备执行幂等键为服务器 UUID v4 `command_id`。
- 设备 telemetry 调整为 QoS 0、`retain=false`；registration 保持 QoS 2、`retain=true` 和 retained 遗嘱。
- V1 面向当前不足 10 台设备稳定运行，多实例高可用、历史遥测和复杂过载机制留待后续设计。

完整设计见 `route_service/docs/2026-07-18-路由服务V1设计.md`。
全局实施路线见 `route_service/docs/2026-07-18-路由服务V1实施计划.md`。
