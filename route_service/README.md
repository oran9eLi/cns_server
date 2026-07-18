# CNS Server / Route Service

CNS（通信、导航、监视）实训箱的设备数据库维护与命令路由核心服务。

本仓库是三端系统中的服务端一环，与 [cns_rpi](https://github.com/oran9eLi/cns_rpi) 仓库配套：

- **RPi 端**（`cns_rpi` 仓库）：从 STM32 收 MAVLink 遥测/身份数据，解码后通过 MQTT 发布。
- **本服务**（`cns_server/route_service`）：作为 MQTT client 订阅 RPi 发布的注册、遥测和 ACK，维护 PostgreSQL 设备数据库并路由命令。
- **STM32 固件**：独立仓库，不在本仓库范围内。

## 职责

V1 阶段核心职责：

1. **设备发现与注册**：订阅 `{namespace}/+/registration` 通配符 topic，接收 RPi 上报的 online/offline 注册消息，以 `vendor_id`（厂商唯一产品识别码，20 字符）为主键维护设备表。
2. **设备状态维护**：online 时更新 `status` 和 `last_seen_at`，offline 时只更新 `status`（遵循注册消息的局部 upsert 语义，缺失字段不清空旧值）。
3. **遥测身份补全**：注册消息中角色号缺失时，后续从同一 `vendor_id` 的遥测身份字段补充 `dcdw_label`，不因字段缺失清空已知值。
4. **命令路由**：登记固定命令来源，按学校名与内部编号或 `vendor_id` 寻址，执行来源权限与设备同校限制，向目标设备发布规范化配置或飞控命令。
5. **幂等与结果回程**：持久化来源 `request_id`、服务器 `command_id` 和目标 ACK 状态，把路由失败或执行结果返回原命令来源。

本服务是设备数据库核心表的唯一写入者。未来的 `backend_service` 只读设备数据，并作为一个固定命令来源接入，不直接修改设备核心表，也不绕过本服务控制设备。

## 数据来源

数据全部来自 RPi 端 MQTT 发布，不依赖人工录入：

| MQTT topic | 方向 | 用途 |
|---|---|---|
| `{namespace}/{vendor_id}/registration` | RPi→本服务 | retained online/offline 注册消息，维护设备表 |
| `{namespace}/{vendor_id}/telemetry` | RPi→本服务 | 更新设备最后活跃时间，并补充注册时缺失的身份字段 |
| `{namespace}/sources/{source_id}/config/request` | 命令来源→本服务 | 提交运行时配置请求 |
| 飞控来源请求 topic（待设计确认） | 命令来源→本服务 | 提交飞控请求，最终由目标 RPi 转为 MAVLink |
| `{namespace}/{vendor_id}/config/ack` | RPi→本服务 | 返回配置应用结果 |
| `{namespace}/{vendor_id}/control/ack` | RPi→本服务 | 返回 STM32 执行结果 |

注册 payload 格式见 `cns_rpi` 仓库 `docs/superpowers/specs/2026-07-10-mqtt-registration-discovery-design.md`；配置命令来源、寻址、权限、幂等和 ACK 协议见 `docs/MQTT命令路由与运行时配置设计.md`，设备侧飞控命令格式见 `docs/下行飞行控制协议.md`。来源侧飞控请求协议仍需在 `route_service` 正式设计中补齐。

## 数据库

引擎：**PostgreSQL**。

### schools（学校信息）

| 字段 | 类型 | 说明 |
|---|---|---|
| `school_id` | BIGSERIAL PK | 自增主键 |
| `school_name` | TEXT UNIQUE NOT NULL | 学校名称，由注册 payload 的 `school_name` 自动创建 |
| `created_at` | TIMESTAMPTZ DEFAULT NOW() | 首次出现该学校时自动记录 |

### devices（设备信息）

| 字段 | 类型 | 说明 |
|---|---|---|
| `vendor_id` | VARCHAR(20) PK | 厂商唯一产品识别码，直接用设备上报值，不加自增 ID |
| `school_id` | BIGINT FK→schools | 所属学校 |
| `dcdw_label` | TEXT | DCDW-XXX 角色号，payload 原样存，初始 NULL（角色号晚于 vendor_id 就绪） |
| `model_version` | TEXT DEFAULT 'CNS v1.0' | 设备型号版本 |
| `status` | TEXT CHECK | online / offline / maintenance，默认 offline |
| `provisioned_at` | TIMESTAMPTZ DEFAULT NOW() | 首次 online 注册时自动写入 |
| `last_seen_at` | TIMESTAMPTZ | 最后一次 online 时间，offline 时不更新 |

约束：
- `devices.school_id` 外键引用 `schools.school_id`
- `UNIQUE(school_id, dcdw_label)` —— 同校内角色号不重复（NULL 之间不冲突，允许角色号未就绪的多台设备共存）

### 注册消息 → 数据库映射

- **online 无 dcdw_label**：UPSERT school + UPSERT device（`dcdw_label` 留 NULL）
- **online 有 dcdw_label**：同上，额外写入 `dcdw_label` 原样字符串
- **offline**：仅 `UPDATE status='offline'`，不触碰其他字段

## 当前状态

- 项目边界已确认。
- 数据库表结构、遥测是否保存历史、离线判定、命令状态机和部署方式仍需在正式设计中确认。
- 实现语言、依赖库和进程结构尚未选定。
- 下一步先写 `route_service` 设计文档，再写实施计划，确认后才开始编码。

## 测试与部署约束

- 日常开发在开发机进行；数据库无关的单元测试可在开发机运行。
- PostgreSQL 数据库、Mosquitto 和 systemd 的集成测试必须在硬件部服务器拉取对应提交后执行。
- 数据库迁移必须版本化、幂等且可重复执行，不得依赖手工修改生产表。
- 测试数据必须使用独立标识并可清理，不得破坏服务器已有设备与命令记录。
- 最终数据库保留在服务器，数据库数据目录、密码和现场配置不进入 Git。
