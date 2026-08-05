# CNS Server / Route Service

CNS（通信、导航、监视）实训箱的设备数据库维护与命令路由核心服务。

本仓库是三端系统中的服务端一环，与 [cns_rpi](https://github.com/oran9eLi/cns_rpi) 仓库配套：

- **RPi 端**（`cns_rpi` 仓库）：从 STM32 收 MAVLink 遥测/身份数据，解码后通过 MQTT 发布。
- **本服务**（`cns_server/route_service`）：作为 MQTT client 订阅 RPi 发布的注册、遥测和 ACK，维护 PostgreSQL 设备数据库并路由命令。
- **STM32 固件**：独立仓库，不在本仓库范围内。

## 职责

V1 阶段核心职责：

1. **设备发现与注册**：订阅 `{namespace}/+/registration` 通配符 topic，接收 RPi 上报的 online/offline 注册消息；统一 `device_id` 支持主控箱 20 位编号和 PX4 UID 派生标识，数据库继续以兼容列名 `vendor_id` 作为主键。
2. **设备状态维护**：online registration 和有效快照 telemetry 更新最后活跃时间；显式 offline 或默认 180 秒无活动超时将设备设为离线，缺失字段不清空旧值。
3. **最新遥测与身份补全**：只保存每台设备最新 JSONB 快照，不保存逐帧历史；registration 是 `dcdw_label` 主来源，遥测只在当前值为空时兜底补全。
4. **命令路由**：登记固定命令来源，按学校名与内部编号或 `vendor_id` 寻址，执行来源权限与设备同校限制，向目标设备发布规范化配置或飞控命令。
5. **幂等与结果回程**：持久化来源 `request_id`、服务器 `command_id` 和目标 ACK 状态，把路由失败或执行结果返回原命令来源。

本服务是设备数据库核心表的唯一写入者。软件部系统通过本服务发布的 retained MQTT 全量设备目录和设备当前状态读取设备数据，无需连接 PostgreSQL；后续控制能力仍通过固定命令来源接入，不直接修改设备核心表，也不绕过本服务控制设备。

## 数据来源

设备注册与遥测来自 RPi 端 MQTT 发布；非设备固定命令来源由现场部署配置声明，不依赖人工直接修改核心表：

| MQTT topic | 方向 | 用途 |
|---|---|---|
| `{namespace}/{device_id}/registration` | RPi→本服务 | retained online/offline 注册消息；兼容主控箱 schema v1 和统一 schema v2 |
| `{namespace}/{device_id}/telemetry/snapshot/v1` | RPi→本服务 | schema v3、QoS 0、非 retained；保存主控箱或 PX4 的最新完整快照 |
| `{namespace}/sources/{source_id}/config/request` | 命令来源→本服务 | 提交运行时配置请求 |
| `{namespace}/sources/{source_id}/control/request` | 命令来源→本服务 | 提交飞控请求，最终由目标 RPi 转为 MAVLink |
| `{namespace}/{vendor_id}/config/ack` | RPi→本服务 | 返回配置应用结果 |
| `{namespace}/{vendor_id}/control/ack` | RPi→本服务 | 返回 STM32 执行结果 |
| `{namespace}/events/devices/directory` | 本服务→软件部 | QoS 1、retained；全部已入库设备的基础资料和在线状态 |
| `{namespace}/events/devices/{vendor_id}/state` | 本服务→软件部 | QoS 1、retained；设备资料、在线状态和最新遥测的完整当前快照 |

注册 payload 格式见 `cns_rpi` 仓库 `docs/superpowers/specs/2026-07-10-mqtt-registration-discovery-design.md`；设备侧配置与飞控协议见关联仓库文档。来源侧飞控请求和统一命令表见 `docs/2026-07-18-路由服务V1设计.md`；面向软件部的最终全量目录和设备状态协议见 `docs/2026-07-28-MQTT全量设备目录对外发布变更设计.md`。

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
| `vendor_id` | VARCHAR(64) PK | 兼容列名，值等于统一 `device_id` |
| `device_type` | TEXT | `cns_box` 或 `flight_controller` |
| `school_id` | BIGINT FK→schools，可空 | 主控箱所属学校；PX4 可暂不绑定 |
| `dcdw_label` | TEXT | DCDW-XXX 角色号，payload 原样存，初始 NULL（角色号晚于 vendor_id 就绪） |
| `model_version` | TEXT DEFAULT 'CNS v1.0' | 设备型号版本 |
| `provisioned_at` | TIMESTAMPTZ DEFAULT NOW() | 首次 online 注册时自动写入 |

约束：
- `devices.school_id` 外键引用 `schools.school_id`
- `UNIQUE(school_id, dcdw_label)` —— 同校内角色号不重复（NULL 之间不冲突，允许角色号未就绪的多台设备共存）

### device_latest_states（设备最新状态）

| 字段 | 类型 | 说明 |
|---|---|---|
| `vendor_id` | VARCHAR(64) PK/FK→devices | 与设备一对一，值等于统一 `device_id` |
| `status` | TEXT CHECK | online / offline |
| `last_seen_at` | TIMESTAMPTZ | 最后有效活动的服务器接收时间 |
| `latest_telemetry` | JSONB | 最新完整遥测快照 |
| `telemetry_received_at` | TIMESTAMPTZ | 对应实时遥测的服务器接收时间 |
| `updated_at` | TIMESTAMPTZ | 状态记录最后写入时间 |

### 注册消息 → 数据库映射

- **online 无 dcdw_label**：UPSERT school + UPSERT device（`dcdw_label` 留 NULL）
- **online 有 dcdw_label**：同上，额外写入 `dcdw_label` 原样字符串
- **offline**：仅更新 `device_latest_states.status='offline'`，不触碰其他字段

高频状态与稳定元数据分表。快照遥测默认每 5 秒合并批量写入，同一设备在周期内只写最后一份；规范化状态事件立即发布，不受数据库批量周期限制。`telemetry/realtime/v1` 不由本服务订阅，也不进入 PostgreSQL。

运行时全量加载设备目录，当前方案面向约 1,000 台以内的规模保持简单。PostgreSQL 冷启动不可用时服务退出；运行中断线时，已登记设备继续维护内存状态并发布降级事件，未登记新设备暂不接纳，数据库线程默认每 5 秒重连并在恢复后补写。

MQTT 连接恢复后会继续确认业务订阅状态。`MQTT连接已恢复` 只表示客户端已连上 Broker；registration、telemetry、来源命令请求和设备 ACK 等业务 topic 全部订阅成功后，才记录 `MQTT业务订阅已恢复`。连接恢复但业务订阅失败时，服务按短周期补订阅，retained registration 会在订阅恢复后重新进入现有注册处理链路。

业务订阅在某次 MQTT 连接上全部就绪后，设备业务线程会发布一次全量设备目录和全部设备当前快照，包括离线设备。设备上线、离线和有效遥测继续增量刷新单设备 retained 快照；只有设备新增、在线状态或目录基础资料变化时才重新发布全量目录，普通遥测不会触发全量目录重发。

## 当前状态

- 里程碑一“工程基础与数据库骨架”已实现并完成本机验收；2026-07-28 已在硬件服务器完成 PostgreSQL 18、Mosquitto 2.0.22、迁移重复执行、数据恢复和 systemd 常驻验证。
- Route Service V1 设计已逐节确认，书面规格见 `docs/2026-07-18-路由服务V1设计.md`。
- 技术方案为 C++23 单体服务，使用 libmosquitto、libpqxx、nlohmann/json、doctest 和 CMake。
- 里程碑二“设备注册与最新状态”已完成代码实现，包含 registration、telemetry、设备在线状态、最新值合并写库、数据库降级恢复和规范化状态事件；书面设计见 `docs/superpowers/specs/2026-07-20-里程碑二设备注册与最新状态设计.md`。
- 干净构建、本机单元测试和一次性临时 PostgreSQL/Mosquitto 真实链路已通过，结果记录在 `docs/change_history/2026-07-20-里程碑二设备注册与最新状态验收.md`。硬件服务器上的构建、数据库、Broker、systemd 和 retained 快照验证已于 2026-07-28 完成；随后已补验树莓派经公网 FRP 上报 registration 和 telemetry、数据库在线状态及后端设备详情。
- 里程碑三“固定来源与配置命令”已完成代码实现、本机自动化测试、独立 PostgreSQL/Mosquitto 真实链路和真实 RPi 配置命令验证，验收记录见 `docs/change_history/2026-07-20-里程碑三固定来源与配置命令验收.md`。现场服务器验证仍待补充。
- 里程碑四“飞控命令路由”已完成代码和本机自动化验收：支持四种飞控请求、QoS 2 非 retained 下发、进度与终态 ACK、30 秒可刷新期限，以及重启后收敛为 `delivery_uncertain` 且不重发。独立真实依赖联调与真实 RPi 飞控验证仍待安全条件和明确授权，实际证据见 `docs/change_history/2026-07-21-里程碑四飞控命令路由验收.md`。
- 里程碑五公网安全接入已完成设计确认：使用 CNS 私有 CA、端到端 MQTT TLS `8883`、每设备独立凭据和 Mosquitto ACL；云服务器 frps 仅做 TCP 透传，最终由现场服务器 frpc 转发到回环地址的 Mosquitto。设计见 `docs/2026-07-21-MQTT公网安全接入与FRP部署设计.md`，尚未开始实施。
- 收尾里程碑“展示链路固化”已用于本机演示链路，并在 2026-07-28 迁移到硬件服务器；正式 TLS/ACL 和前端体验优化仍不属于该里程碑。
- 面向软件部的 MQTT 只读设备数据接口已升级为全量设备目录：目录与单设备状态均为 QoS 1 retained，目录包含全部已入库设备及在线状态，启动或 MQTT 重连后重发全部设备快照；MQTT 对外 JSON 不暴露内部 `school_id`，重放状态使用 `change_reason=snapshot_replay`。2026-07-28 已在硬件服务器部署并通过本机 Broker 与公网 FRP 入口验证，旧在线目录 retained 已清理；设计见 `docs/2026-07-28-MQTT全量设备目录对外发布变更设计.md`。
- 硬件服务器部署记录见 `docs/change_history/2026-07-28-硬件服务器迁移验收.md`。当前局域网入口为 `http://192.168.11.3/`，树莓派 MQTT 演示入口为 `112.124.52.232:1883`，由硬件服务器 FRP 转发到本机 Broker；真实命令闭环尚未完成，不得写成已验收。

本子项目全局设计和全局计划放在 `docs/` 根目录。当前并行开发期间，Route Service 每个里程碑的设计和详细计划直接在长期 `route_service` 分支编写，分别放在 `docs/superpowers/specs/` 和 `docs/superpowers/plans/`；计划确认后才从最新 `route_service` 建立隔离实施工作树。验收记录在实施工作树的 `docs/change_history/` 编写，完成验收并合入长期 `route_service` 分支后才进入下一里程碑。文件名统一使用“`YYYY-MM-DD-中文主题.md`”。何时把长期分支合回 `main` 由用户统一协调，不在功能工作树中自行处理。

## 测试与部署约束

- 日常开发在开发机进行；数据库无关的单元测试可在开发机运行。
- PostgreSQL 与 Mosquitto 可在本机独立测试环境执行非破坏性联调；现场 PostgreSQL、Mosquitto 和 systemd 集成测试必须在硬件部服务器拉取对应提交且获得用户授权后执行，2026-07-28 首次现场部署已按此要求完成。
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

里程碑二真实依赖联调脚本要求显式传入测试配置、迁移目录和 Broker，不启动或停止系统服务。测试配置必须指向允许创建独立测试标识的数据库：

```bash
route_service/tests/integration/里程碑二本机联调.sh \
  --config /仓库外/route_service.test.json \
  --migrations "$(pwd)/route_service/migrations" \
  --broker-host 127.0.0.1 \
  --broker-port 18884 \
  --binary "$(pwd)/route_service/build-fresh-m2/route_service"
```

从仓库根目录执行且使用默认干净构建路径时可以省略 `--binary`。只验证默认路径、参数、工具和配置，不连接真实依赖：

```bash
route_service/tests/integration/里程碑二本机联调.sh \
  --config /仓库外/route_service.test.json \
  --migrations "$(pwd)/route_service/migrations" \
  --broker-host 127.0.0.1 \
  --broker-port 18884 \
  --check-only
```

脚本定向清理自身创建的设备、来源、最新状态及其命令，仅在学校已无设备时删除测试学校；不会执行 `TRUNCATE`、删除数据库或停止 Broker/PostgreSQL。数据库断线恢复测试需使用另行确认的可控代理环境，不得通过停止共享系统服务制造故障。

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

当前能力包括工程与数据库骨架、设备注册和最新状态，以及配置与飞控命令共用的固定来源、寻址、权限、幂等、QoS 2 下发、设备 ACK、超时和终态清理。配置命令重启后可按原 `command_id` 恢复；飞控命令重启后不重发，非终态统一收敛为 `delivery_uncertain`。

### 当前本机展示链路

当前展示环境使用本机作为临时服务器：

```text
树莓派 → 云端 frps → 本机 frpc → 本机 Mosquitto 18884 → route_service → PostgreSQL → backend_service → 前端
```

相关服务：

- `mosquitto-18884.service`：本机测试 Broker，监听 `18884`。
- `cns-frpc.service`：把云端 MQTT 入口转发到本机 Broker。
- `cns-route-service.service`：路由服务。
- `cns-backend-service.service`：后端服务。

查看 route_service 日志：

```bash
journalctl -u cns-route-service.service -f
```

本链路使用匿名明文 MQTT，仅用于展示和开发验证；正式 TLS/ACL/8883 部署后续单独实施。Broker 晚于 route_service 启动时，应看到 `MQTT连接已恢复` 后继续出现 `MQTT业务订阅已恢复`，否则设备 retained registration 不会可靠进入注册链路。

里程碑三非破坏性联调入口：

```bash
route_service/tests/integration/里程碑三配置命令本机联调.sh \
  --config /仓库外/route_service.test.json \
  --migrations "$(pwd)/route_service/migrations" \
  --broker-host 127.0.0.1 --broker-port 18884 \
  --binary "$(pwd)/route_service/build-fresh-m3/route_service" \
  --database-url 'postgresql://测试用户:测试密码@127.0.0.1:测试端口/测试库' \
  --source-id web-console --vendor-id A1b2C3d4E5f6G7h8I9j0
```
