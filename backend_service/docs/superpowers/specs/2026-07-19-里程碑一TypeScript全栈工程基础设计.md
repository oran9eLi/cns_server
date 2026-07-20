# 里程碑一：TypeScript 全栈工程基础设计

版本：2026-07-19

状态：已逐节确认，书面规格已复核

## 1. 目标

本里程碑在 route_service 仍处于早期开发时，先建立可安装、可构建、可测试的 TypeScript 全栈工程，并交付一个可在浏览器查看和操作的 CNS 设备管理页面。

页面使用独立开发模拟器通过正式 REST/WebSocket 契约提供开发数据，覆盖设备列表、详情、实时遥测、运行时配置、飞控控制和命令状态。前端不得依赖 React 内部硬编码数据，也不得在后续接入真实 backend_service 时重写页面组件。

本里程碑优先确认真实可运行页面的布局、品牌和交互质量。最终字体、图标、间距、阴影和动效在页面运行后继续优化，不在静态线框阶段过早锁死。

## 2. 范围

### 2.1 包含

- 根目录 npm workspaces 和单一锁文件。
- backend_service 的 Node.js、TypeScript、Fastify 工程骨架、健康检查、配置、日志和测试基础。
- `backend_service/protocol` 正式 REST/WebSocket schema 包。
- frontend 的 React、Vite、TypeScript、Ant Design、TanStack Query 工程。
- 可查看的设备列表页和独立设备详情控制页。
- 原生 WebSocket 客户端、重连和实时数据合并。
- 独立 `frontend/dev-simulator` REST/WebSocket 模拟服务。
- 设备、遥测、命令和故障开发场景。
- 公司现有 Logo 与 `#132B88` 品牌主题。
- 本机自动化测试、生产构建和开发数据隔离验证。

### 2.2 不包含

- PostgreSQL、Mosquitto 或 route_service 真实连接。
- route_service 命令、数据库和 MQTT 集成验证。
- Nginx、Basic Auth、HTTPS、systemd 或现场部署。
- 应用账号、JWT、学校权限、审计和命令历史。
- 遥测历史、趋势图和 ECharts。
- 最终视觉打磨和移动端专项适配。
- 任何现场服务器状态修改。

## 3. 工程结构

根目录只保留三个产品子项目，避免增加含义宽泛的顶层 `shared/` 或 `tools/`：

```text
cns_server/
├─ route_service/
├─ backend_service/
│  ├─ src/
│  │  ├─ config/
│  │  ├─ logging/
│  │  └─ server.ts
│  ├─ protocol/
│  │  ├─ src/
│  │  └─ package.json
│  ├─ test/
│  └─ package.json
└─ frontend/
   ├─ src/
   │  ├─ app/
   │  ├─ api/
   │  ├─ realtime/
   │  ├─ devices/
   │  ├─ commands/
   │  └─ assets/
   ├─ dev-simulator/
   │  ├─ src/
   │  │  ├─ fixtures/
   │  │  ├─ scenarios/
   │  │  └─ server.ts
   │  ├─ test/
   │  └─ package.json
   ├─ test/
   └─ package.json
```

根 npm workspaces 识别 backend_service、协议包、frontend 和开发模拟器。根命令提供统一安装、构建、测试、类型检查和开发启动入口。

开发命令同时启动：

```text
npm run dev
  ├─ frontend：Vite开发服务器
  ├─ backend_service：Fastify工程骨架与健康检查
  └─ frontend/dev-simulator：开发数据REST/WebSocket服务
```

任一子进程启动失败时，根命令明确报错并统一退出，不留下难以发现的半启动状态。

## 4. 组件与依赖边界

### 4.1 backend_service

里程碑一只实现工程生命周期、配置、结构化日志和 `GET /api/health`。它不实现设备、命令、数据库或 MQTT 业务，也不提供开发模拟接口。

配置使用 Zod 严格校验。日志使用结构化 JSON 输出到 stdout/stderr，中文消息与英文协议标识并存；密码、凭据和完整敏感配置不得输出。

### 4.2 正式协议包

`backend_service/protocol` 由 backend_service 拥有，定义其对浏览器公开的 REST/WebSocket Zod schema 和 TypeScript 类型。backend_service、frontend 和 dev-simulator 都可以依赖该包。

协议包不得依赖 Fastify、React、MQTT.js、PostgreSQL 或模拟器。浏览器协议不得暴露 MQTT topic 或设备端内部结构。

### 4.3 frontend

frontend 始终通过正式 REST 客户端和原生 WebSocket 客户端获取数据。页面组件只消费经协议 schema 验证的页面模型，不判断对端是模拟器还是真实 backend_service。

TanStack Query 管理 REST 快照、缓存、加载和错误；WebSocket 管理器负责连接、重连、事件校验和增量分发，并将设备事件按时间戳合并到 Query Cache。命令状态保存在详情页局部状态。

### 4.4 dev-simulator

dev-simulator 是独立 workspace，只绑定本机回环地址。它与 backend_service 不互相导入，只共同依赖正式协议包。

模拟器使用固定随机种子产生轻微遥测变化，使自动化测试可重复。它不自动触发飞控操作，不频繁产生通知，也不把模拟结果描述成真实设备 ACK。

## 5. 正式浏览器协议

正式 REST/WebSocket 契约为：

```text
GET  /api/health
GET  /api/devices
GET  /api/devices/:vendor_id
POST /api/devices/:vendor_id/commands
WS   /ws
```

设备列表、详情和命令结构遵循 Backend Service V1 全局设计。浏览器生成 `client_request_id`，并在命令请求中携带当前 WebSocket `session_id`。

WebSocket 至少支持：

- `session.ready`
- `device.state`
- `command.updated`

所有正式消息使用 `schema_version=1`，时间为 UTC RFC 3339 毫秒格式。命令结果按 `session_id` 只推送给提交页面，设备状态广播给所有连接。

## 6. 开发模拟器协议与场景

模拟器实现全部正式设备、命令和 WebSocket 接口。开发场景接口使用独立前缀：

```text
POST /__dev/scenarios/device-state
POST /__dev/scenarios/service-state
POST /__dev/scenarios/next-command-result
POST /__dev/scenarios/websocket
POST /__dev/scenarios/reset
```

`/__dev/` 不属于正式协议包，不得在 backend_service 中实现。frontend 的开发工具条是这些接口的唯一消费者。

开发数据至少包含 8 台设备和两所学校，并覆盖：

- 在线、离线和降级状态。
- 有与没有 `dcdw_label`。
- 最近活跃与长时间离线。
- 完整遥测与部分字段缺失。

模拟器支持主动触发：

- 指定设备上线或离线。
- route_service 降级与恢复。
- WebSocket 中断与重连。
- 下一条命令成功、拒绝、超时或 `delivery_uncertain`。
- 恢复初始开发数据。

命令默认按以下过程推送：

```text
submitted → dispatched → in_progress → succeeded
```

开发工具条可改变下一条命令的终态。模拟器执行会话级路由，不能把命令结果广播给其他浏览器连接。

## 7. 开发与生产隔离

页面使用模拟器时持续显示“开发数据”标识。开发工具条只在开发构建中存在，可折叠，不占用正式客户页面的固定布局。

必须同时满足：

- 生产构建通过编译期条件完全移除开发工具条代码。
- 生产构建中不存在对 `/__dev/` 的调用。
- 生产配置校验拒绝模拟器地址或开发数据标志。
- dev-simulator 不被 backend_service 或 frontend 生产构建打包。
- 开发数据与真实 API 使用同一正式页面模型。

## 8. 页面信息架构与交互

### 8.1 页面框架

V1 使用：

```text
/devices
/devices/:vendor_id
```

页面面向内部人员和客户共同使用，采用明亮、专业、现代的企业级管理界面，不采用内部诊断工具式的粗糙高密度布局。

整体以 1440px 桌面屏为主要设计尺寸，保证 1280px 可完整操作。左侧为约 208px 固定导航；顶部内容区显示页面标题、WebSocket 状态和服务状态。V1 不做复杂多级菜单。

### 8.2 设备列表页

列表页采用：

```text
统计卡片
  → 搜索与学校/在线状态筛选
  → 设备表格
```

支持搜索 `vendor_id`、角色号和学校。WebSocket 事件直接更新相应设备，不整页刷新。点击表格行或详情操作进入独立详情页。

### 8.3 设备详情页

详情页顶部显示设备身份、状态、学校、型号和最后活跃时间。主体采用左右双栏：左侧遥测约 65%，右侧控制约 35%；空间不足时变为单列，遥测在上、控制在下。

左侧展示姿态、环境、链路和电机核心卡片，并提供完整键值视图与格式化 JSON 视图。字段缺失显示 `--`，不得伪造默认值。

右侧提供：

- 四项运行时配置参数。
- 四路 PWM。
- `takeoff`、`land`、`emergency_stop`。
- 当前命令的连续状态、业务状态和错误信息。

命令提交期间禁用其他命令。HTTP 提交失败与设备/路由执行失败必须分别展示。

## 9. 品牌与视觉基线

- Ant Design `colorPrimary` 使用 `#132B88`。
- 白色是主要内容面，浅灰蓝是页面背景。
- 绿色、红色、黄色分别表示成功/在线、失败/离线、警告/降级。
- 侧边栏使用 `/home/oran9e/桌面/东创logo/透明底logo（适用任何背景）/透明logo3 .png` 的圆形徽标。
- 欢迎、空状态或需要完整品牌展示时可使用同目录 `透明logo1.png`。
- 实施时把所需 Logo 原样复制到 `frontend/src/assets/`，不使用 AI 重新生成或修改公司标识。
- 不使用渐变、玻璃拟态、发光描边或大面积装饰纹理。
- 圆角、阴影和胶囊标签保持克制，具体数值在可运行页面中调整。

先固定页面结构与品牌气质，不把缩小线框图视为最终视觉稿。真实 React 页面完成后再评审字体、图标、间距、阴影和细节质感。

## 10. 动效与状态反馈

- 页面进入、侧栏和折叠区域使用短促淡入或位移。
- 实时数据只对变化字段做轻微高亮，不让整张卡片跳动。
- 命令状态使用连续步骤反馈，不用无限旋转动画掩盖超时。
- 在线状态不持续闪烁，只在连接恢复时短暂提示。
- 尊重 `prefers-reduced-motion`，减少动画时禁用非必要过渡。
- 具体时长和缓动曲线在可运行页面中评审，不在本设计锁死。

## 11. 故障与错误处理

- backend_service、frontend 或 simulator 任一开发进程启动失败时，统一开发命令明确失败。
- REST 响应、WebSocket 事件和开发 fixture 均经过 Zod 校验。
- 非法单条事件被记录并忽略，不使页面或模拟器崩溃。
- WebSocket 断开时页面持续显示警告并禁止命令；自动重连后重新读取 REST 快照。
- 旧时间戳事件不得覆盖新设备状态。
- 命令拒绝、超时和结果不确定使用不同中文提示，并保留原始业务错误码。
- 未找到设备时显示明确空状态，并提供返回列表入口。
- 核心遥测字段缺失显示 `--`，完整 JSON 仍展示实际收到的结构。

## 12. 测试与验收

### 12.1 自动化测试

覆盖：

- 根 workspace 安装、构建、类型检查和测试。
- 正式 REST/WebSocket schema 的合法与非法样本。
- backend_service 配置、日志脱敏、健康检查和正常退出。
- simulator 设备查询、筛选、场景切换和命令状态机。
- WebSocket 会话级命令结果路由。
- 前端设备列表筛选、详情跳转和字段缺失。
- 配置与飞控表单结构、边界和按钮禁用。
- WebSocket 数据合并、断线、重连和旧事件丢弃。
- 开发工具条场景触发。
- 生产构建中不存在开发工具条和 `/__dev/` 调用。

### 12.2 人工验收

1. 一条根命令启动 backend_service 骨架、模拟器和前端。
2. 页面显示公司 Logo、`#132B88` 主题和“开发数据”标识。
3. 设备列表、筛选和独立详情控制页可完整操作。
4. 模拟遥测变化无需刷新即可更新。
5. 四项配置和四类飞控操作具有连续命令反馈。
6. 可主动演示成功、拒绝、超时、结果不确定、设备离线、服务降级和 WebSocket 重连。
7. 1280px 和 1440px 桌面宽度下无关键内容遮挡。
8. 生产构建通过，且无法启用开发数据。

## 13. 开发流程

本设计和对应详细实施计划直接在 `main` 确认并提交。实施计划确认后，使用 `superpowers:using-git-worktrees` 从最新 `main` 建立 backend_service 里程碑一独立工作树和功能分支，再按 TDD 实施。

route_service 可以继续在自己的工作树并行开发。本里程碑不读取或依赖 route_service 工作树中的临时代码，不修改现场服务器状态。
