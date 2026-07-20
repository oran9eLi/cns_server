# CNS Server / Route Service

CNS（通信、导航、监视）实训箱的设备数据库维护与命令路由核心服务。

本仓库是三端系统中的服务端一环，与 [cns_rpi](https://github.com/oran9eLi/cns_rpi) 仓库配套：

- **RPi 端**（`cns_rpi` 仓库）：从 STM32 收 MAVLink 遥测/身份数据，解码后通过 MQTT 发布。
- **本服务**（`cns_server/route_service`）：作为 MQTT client 订阅 RPi 发布的注册、遥测和 ACK，维护 PostgreSQL 设备数据库并路由命令。
- **STM32 固件**：独立仓库，不在本仓库范围内。

## 职责

V1 阶段核心职责：

1. **设备发现与注册**：订阅 `{namespace}/+/registration` 通配符 topic，接收 RPi 上报的 online/offline 注册消息，以 `vendor_id`（厂商唯一产品识别码，20 字符）为主键维护设备表。
2. **设备状态维护**：online registration 和有效实时 telemetry 更新最后活跃时间；显式 offline 或默认 180 秒无活动超时将设备设为离线，缺失字段不清空旧值。
3. **最新遥测与身份补全**：只保存每台设备最新 JSONB 快照，不保存逐帧历史；registration 是 `dcdw_label` 主来源，遥测只在当前值为空时兜底补全。
4. **命令路由**：登记固定命令来源，按学校名与内部编号或 `vendor_id` 寻址，执行来源权限与设备同校限制，向目标设备发布规范化配置或飞控命令。
5. **幂等与结果回程**：持久化来源 `request_id`、服务器 `command_id` 和目标 ACK 状态，把路由失败或执行结果返回原命令来源。

本服务是设备数据库核心表的唯一写入者。未来的 `backend_service` 只读设备数据、订阅本服务输出的规范化实时状态事件，并作为一个固定命令来源接入，不直接修改设备核心表、不把原始设备 telemetry 作为权威数据源，也不绕过本服务控制设备。

## 数据来源

设备注册与遥测来自 RPi 端 MQTT 发布；非设备固定命令来源由现场部署配置声明，不依赖人工直接修改核心表：

| MQTT topic | 方向 | 用途 |
|---|---|---|
| `{namespace}/{vendor_id}/registration` | RPi→本服务 | retained online/offline 注册消息，维护设备表 |
| `{namespace}/{vendor_id}/telemetry` | RPi→本服务 | QoS 0、非 retained；更新最新快照和活跃时间，并在角色号为空时兜底补全 |
| `{namespace}/sources/{source_id}/config/request` | 命令来源→本服务 | 提交运行时配置请求 |
| `{namespace}/sources/{source_id}/control/request` | 命令来源→本服务 | 提交飞控请求，最终由目标 RPi 转为 MAVLink |
| `{namespace}/{vendor_id}/config/ack` | RPi→本服务 | 返回配置应用结果 |
| `{namespace}/{vendor_id}/control/ack` | RPi→本服务 | 返回 STM32 执行结果 |
| `{namespace}/events/devices/{vendor_id}/state` | 本服务→后端 | QoS 0、非 retained 的规范化实时状态事件 |

注册 payload 格式见 `cns_rpi` 仓库 `docs/superpowers/specs/2026-07-10-mqtt-registration-discovery-design.md`；设备侧配置与飞控协议见关联仓库文档。来源侧飞控请求、统一命令表和实时状态事件协议见 `docs/2026-07-18-路由服务V1设计.md`。

## 数据库

引擎：**PostgreSQL**。

### schools（学校信息）

| 字段 | 类型 | 说明 |
|---|---|---|
| `school_id` | BIGSERIAL PK | 自增主键 |
| `school_name` | TEXT UNIQUE NOT NULL | 学校名称，由注册 payload 的 `school_name` 自动创建 |
| `created_at` | TIMESTAMPTZ DEFAULT NOW() | 首次出现该学校时自动记录 |

### devices（稳定设备信息）

| 字段 | 类型 | 说明 |
|---|---|---|
| `vendor_id` | VARCHAR(20) PK | 厂商唯一产品识别码，直接用设备上报值，不加自增 ID |
| `school_id` | BIGINT FK→schools | 所属学校 |
| `dcdw_label` | TEXT | DCDW-XXX 角色号，payload 原样存，初始 NULL（角色号晚于 vendor_id 就绪） |
| `model_version` | TEXT DEFAULT 'CNS v1.0' | 设备型号版本 |
| `provisioned_at` | TIMESTAMPTZ DEFAULT NOW() | 首次 online 注册时自动写入 |

约束：
- `devices.school_id` 外键引用 `schools.school_id`
- `UNIQUE(school_id, dcdw_label)` —— 同校内角色号不重复（NULL 之间不冲突，允许角色号未就绪的多台设备共存）

### device_latest_states（设备最新状态）

| 字段 | 类型 | 说明 |
|---|---|---|
| `vendor_id` | VARCHAR(20) PK/FK→devices | 与设备一对一 |
| `status` | TEXT CHECK | online / offline |
| `last_seen_at` | TIMESTAMPTZ | 最后有效活动的服务器接收时间 |
| `latest_telemetry` | JSONB | 最新完整遥测快照 |
| `telemetry_received_at` | TIMESTAMPTZ | 对应实时遥测的服务器接收时间 |
| `updated_at` | TIMESTAMPTZ | 状态记录最后写入时间 |

### 注册消息 → 数据库映射

- **online 无 dcdw_label**：UPSERT school + UPSERT device（`dcdw_label` 留 NULL）
- **online 有 dcdw_label**：同上，额外写入 `dcdw_label` 原样字符串
- **offline**：仅更新 `device_latest_states.status='offline'`，不触碰其他字段

高频状态与稳定元数据分表。遥测默认每 5 秒合并批量写入，同一设备在周期内只写最后一份；实时状态事件立即发布，不受数据库批量周期限制。

## 当前状态

- 里程碑一“工程基础与数据库骨架”已实现并完成本机验收；现场 PostgreSQL、Mosquitto、迁移重复执行和信号退出尚未验证，等待用户授权。
- Route Service V1 设计已逐节确认，书面规格见 `docs/2026-07-18-路由服务V1设计.md`。
- 技术方案为 C++23 单体服务，使用 libmosquitto、libpqxx、nlohmann/json、doctest 和 CMake。
- 书面规格已复核，全局里程碑路线见 `docs/2026-07-18-路由服务V1实施计划.md`；当前尚未实现 registration、telemetry、ACK、业务 topic、设备在线状态、遥测合并写库、实时状态事件及配置/飞控命令路由。

本子项目全局设计和全局计划放在 `docs/` 根目录。每个里程碑的设计和详细计划直接在 `main` 编写，分别放在 `docs/superpowers/specs/` 和 `docs/superpowers/plans/`；计划确认后才从最新 `main` 建立实施工作树。验收记录在实施工作树的 `docs/change_history/` 编写，完成验收并合入 `main` 后才进入下一里程碑。文件名统一使用“`YYYY-MM-DD-中文主题.md`”。

## 测试与部署约束

- 日常开发在开发机进行；数据库无关的单元测试可在开发机运行。
- PostgreSQL 数据库、Mosquitto 和 systemd 的集成测试必须在硬件部服务器拉取对应提交后执行。
- 数据库迁移必须版本化、幂等且可重复执行，不得依赖手工修改生产表。
- 测试数据必须使用独立标识并可清理，不得破坏服务器已有设备与命令记录。
- 最终数据库保留在服务器，数据库数据目录、密码和现场配置不进入 Git。

## Ubuntu 构建与本机测试

安装系统依赖（Ubuntu 包名）：

```bash
sudo apt install build-essential cmake pkg-config binutils libmosquitto-dev libpqxx-dev nlohmann-json3-dev doctest-dev
```

本工程不通过 CMake 联网下载依赖。当前兼容层要求 Linux、GNU 工具链、binutils 提供的 `readelf`、libmosquitto 2.0.22 和 libpqxx 7.10.x。

从仓库根目录执行干净构建和全部本机测试：

```bash
cmake -S route_service -B route_service/build-fresh -DCMAKE_BUILD_TYPE=Debug
cmake --build route_service/build-fresh -j2
ctest --test-dir route_service/build-fresh --output-on-failure
```

## 配置与运行

先由用户确认专用服务账户和组，再以该身份复制示例配置到仓库外的现场路径并替换数据库密码等部署值；不得把真实配置提交到 Git。下面两个占位值必须先替换，配置文件所有者与运行进程身份必须一致：

```bash
service_user='<用户确认的专用服务账户>'
service_group='<用户确认的专用服务组>'
sudo install -d -o "$service_user" -g "$service_group" -m 0750 /etc/cns
sudo install -o "$service_user" -g "$service_group" -m 0600 \
  route_service/config/route_service.example.json /etc/cns/route_service.json
```

显式迁移只在 `--migrate-only` 模式执行：

```bash
service_user='<用户确认的专用服务账户>'
sudo -u "$service_user" route_service/build-fresh/route_service \
  --config /etc/cns/route_service.json \
  --migrations "$(pwd)/route_service/migrations" \
  --migrate-only
```

正常模式只检查数据库迁移版本，不自动修改数据库：

```bash
service_user='<用户确认的专用服务账户>'
sudo -u "$service_user" route_service/build-fresh/route_service \
  --config /etc/cns/route_service.json \
  --migrations "$(pwd)/route_service/migrations"
```

里程碑一当前能力包括严格配置与命令行解析、中文单行日志、有界队列、PostgreSQL 只向前迁移、正常/仅迁移模式、MQTT 自动重连骨架和信号有序退出。它不包含任何设备 registration、telemetry、ACK、业务订阅/发布、在线状态、遥测写库或命令路由功能。
