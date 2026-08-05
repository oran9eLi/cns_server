#!/usr/bin/env bash
# 本脚本只创建并清理带有独立测试标识的数据，不得用于生产配置。
set -euo pipefail

usage() {
  cat <<'EOF'
用法：里程碑二本机联调.sh --config <测试配置> --migrations <迁移目录> \
  --broker-host <地址> --broker-port <端口> [--binary <route_service>] \
  [--device-id <20字符编号>] [--check-only]

要求：测试配置必须指向允许写入测试标识的本机数据库；脚本不会启动、停止或修改系统服务。
`--check-only` 只检查参数、工具、配置和文件路径，不连接数据库或 Broker。
EOF
}

config_file=''
migrations_dir=''
broker_host=''
broker_port=''
binary=''
device_id=''
check_only=false
while (($# > 0)); do
  case "$1" in
    --config) config_file=${2-}; shift 2 ;;
    --migrations) migrations_dir=${2-}; shift 2 ;;
    --broker-host) broker_host=${2-}; shift 2 ;;
    --broker-port) broker_port=${2-}; shift 2 ;;
    --binary) binary=${2-}; shift 2 ;;
    --device-id) device_id=${2-}; shift 2 ;;
    --check-only) check_only=true; shift ;;
    --help|-h) usage; exit 0 ;;
    *) echo "错误：未知参数 $1" >&2; usage >&2; exit 2 ;;
  esac
done

[[ -n $config_file && -n $migrations_dir && -n $broker_host && -n $broker_port ]] || {
  echo '错误：必须显式传入测试配置、迁移目录和 Broker。' >&2
  usage >&2
  exit 2
}
[[ -r $config_file && -d $migrations_dir ]] || { echo '错误：配置或迁移目录不可读。' >&2; exit 2; }
for tool in jq psql pg_isready mosquitto_pub mosquitto_sub sha256sum; do
  command -v "$tool" >/dev/null || { echo "错误：缺少工具 $tool。" >&2; exit 2; }
done

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_dir=$(cd -- "$script_dir/../../.." && pwd)
binary=${binary:-$repo_dir/route_service/build-fresh-m2/route_service}
[[ -x $binary ]] || { echo "错误：程序不可执行：$binary" >&2; exit 2; }

if [[ -z $device_id ]]; then
  digest=$(printf '%s' "$$-$(date +%s%N)" | sha256sum)
  device_id="M2${digest:0:18}"
  device_id=${device_id^^}
fi
[[ $device_id =~ ^[A-Za-z0-9]{20}$ ]] || { echo '错误：device_id 必须是 20 个 ASCII 字母或数字。' >&2; exit 2; }
unknown_device="U${device_id:1}"
school_name="CNS_M2_LOCAL_${device_id}"
dcdw_label="DCDW-M2-${device_id: -6}"
topic_namespace=$(jq -er '.mqtt.topic_namespace' "$config_file")
offline_seconds=$(jq -er '.device_state.offline_timeout_seconds' "$config_file")

database_host=$(jq -er '.database.host' "$config_file")
database_port=$(jq -er '.database.port' "$config_file")
database_name=$(jq -er '.database.name' "$config_file")
database_user=$(jq -er '.database.user' "$config_file")
export PGPASSWORD
PGPASSWORD=$(jq -er '.database.password' "$config_file")
PSQL=(psql -X -v ON_ERROR_STOP=1 -h "$database_host" -p "$database_port" -U "$database_user" -d "$database_name")
MQTT=(-h "$broker_host" -p "$broker_port")

if $check_only; then
  echo "检查通过：将使用路由服务程序 $binary"
  unset PGPASSWORD
  exit 0
fi

service_pid=''
event_pid=''
temp_dir=$(mktemp -d)
service_log="$temp_dir/route_service.log"
event_log="$temp_dir/events.jsonl"

cleanup() {
  local exit_code=$?
  trap - EXIT INT TERM
  [[ -z $event_pid ]] || kill "$event_pid" 2>/dev/null || true
  [[ -z $service_pid ]] || kill -TERM "$service_pid" 2>/dev/null || true
  [[ -z $service_pid ]] || wait "$service_pid" 2>/dev/null || true
  mosquitto_pub "${MQTT[@]}" -q 2 -r -t "$topic_namespace/$device_id/registration" -n 2>/dev/null || true
  mosquitto_pub "${MQTT[@]}" -q 2 -r -t "$topic_namespace/$unknown_device/registration" -n 2>/dev/null || true
  "${PSQL[@]}" -v device="$device_id" -v school="$school_name" >/dev/null 2>&1 <<'SQL' || true
DELETE FROM commands WHERE source_id = :'device' OR target_device_id = :'device';
DELETE FROM command_sources WHERE source_id = :'device' OR device_id = :'device';
DELETE FROM device_latest_states WHERE device_id = :'device';
DELETE FROM devices WHERE device_id = :'device';
DELETE FROM schools s WHERE s.school_name = :'school'
  AND NOT EXISTS (SELECT 1 FROM devices d WHERE d.school_id = s.school_id);
SQL
  if ((exit_code != 0)); then
    echo '联调失败，route_service 安全诊断如下：' >&2
    sed -n '1,240p' "$service_log" >&2 2>/dev/null || true
  fi
  rm -rf -- "$temp_dir"
  unset PGPASSWORD
  exit "$exit_code"
}
trap cleanup EXIT INT TERM

wait_for_sql() {
  local query=$1 expected=$2 deadline=$((SECONDS + 15)) actual=''
  while ((SECONDS < deadline)); do
    actual=$("${PSQL[@]}" -Atqc "$query" 2>/dev/null || true)
    [[ $actual == "$expected" ]] && return 0
    sleep 0.2
  done
  echo "错误：等待数据库条件超时，实际值=$actual" >&2
  return 1
}

wait_for_event() {
  local reason=$1 deadline=$((SECONDS + 15))
  while ((SECONDS < deadline)); do
    jq -e --arg device "$device_id" --arg reason "$reason" \
      'select(.device_id == $device and .change_reason == $reason)' "$event_log" >/dev/null 2>&1 && return 0
    sleep 0.2
  done
  echo "错误：等待状态事件超时：$reason" >&2
  return 1
}

pg_isready -h "$database_host" -p "$database_port" -d "$database_name" -U "$database_user" >/dev/null || {
  echo '错误：测试 PostgreSQL 不可用；脚本不会修改系统服务。' >&2
  exit 1
}
mosquitto_pub "${MQTT[@]}" -t "$topic_namespace/integration/probe" -n >/dev/null || {
  echo '错误：测试 Broker 不可用；脚本不会修改系统服务。' >&2
  exit 1
}

echo '验证迁移首次执行及重复执行幂等……'
"$binary" --config "$config_file" --migrations "$migrations_dir" --migrate-only
"$binary" --config "$config_file" --migrations "$migrations_dir" --migrate-only

mosquitto_sub "${MQTT[@]}" -q 0 -t "$topic_namespace/events/devices/+/state" >"$event_log" &
event_pid=$!

echo '验证 retained registration 建档重放与四表记录……'
mosquitto_pub "${MQTT[@]}" -q 2 -r -t "$topic_namespace/$device_id/registration" \
  -m "{\"schema_version\":3,\"device_id\":\"$device_id\",\"device_type\":\"cns_box\",\"school_name\":\"$school_name\",\"dcdw_label\":\"$dcdw_label\",\"status\":\"online\",\"capabilities\":[\"telemetry\"]}"
"$binary" --config "$config_file" --migrations "$migrations_dir" >"$service_log" 2>&1 &
service_pid=$!
wait_for_sql "SELECT count(*) FROM schools s JOIN devices d USING (school_id) JOIN device_latest_states l USING (device_id) JOIN command_sources c ON c.device_id=d.device_id WHERE d.device_id='$device_id' AND c.source_kind='device'" 1
wait_for_event registration_online

echo '验证未知设备 telemetry 被拒绝……'
mosquitto_pub "${MQTT[@]}" -q 0 -t "$topic_namespace/$unknown_device/telemetry/snapshot/v1" \
  -m "{\"schema_version\":3,\"device_id\":\"$unknown_device\",\"device_type\":\"cns_box\",\"sent_at\":\"2026-08-03T10:00:00Z\",\"telemetry\":{\"sequence\":0},\"drone_id\":{\"basic_id\":{\"id_type\":1,\"ua_type\":2}}}"
sleep 1
[[ $("${PSQL[@]}" -Atqc "SELECT count(*) FROM devices WHERE device_id='$unknown_device'") == 0 ]]

echo '验证即时事件、5 秒最后值与 telemetry 非 retained……'
mosquitto_pub "${MQTT[@]}" -q 0 -t "$topic_namespace/$device_id/telemetry/snapshot/v1" \
  -m "{\"schema_version\":3,\"device_id\":\"$device_id\",\"device_type\":\"cns_box\",\"sent_at\":\"2026-08-03T10:00:01Z\",\"telemetry\":{\"sequence\":1},\"drone_id\":{\"basic_id\":{\"id_type\":1,\"ua_type\":2}}}"
wait_for_event telemetry
mosquitto_pub "${MQTT[@]}" -q 0 -t "$topic_namespace/$device_id/telemetry/snapshot/v1" \
  -m "{\"schema_version\":3,\"device_id\":\"$device_id\",\"device_type\":\"cns_box\",\"sent_at\":\"2026-08-03T10:00:02Z\",\"telemetry\":{\"sequence\":2},\"drone_id\":{\"basic_id\":{\"id_type\":1,\"ua_type\":2}}}"
sleep 7
wait_for_sql "SELECT latest_telemetry #>> '{telemetry,sequence}' FROM device_latest_states WHERE device_id='$device_id'" 2
if mosquitto_sub "${MQTT[@]}" -q 0 -t "$topic_namespace/$device_id/telemetry/snapshot/v1" -C 1 -W 2 >"$temp_dir/replayed-telemetry" 2>/dev/null; then
  echo '错误：新订阅者收到旧 telemetry，设备 topic 仍为 retained。' >&2
  exit 1
fi

echo '验证显式离线及活动恢复……'
mosquitto_pub "${MQTT[@]}" -q 2 -r -t "$topic_namespace/$device_id/registration" \
  -m "{\"schema_version\":3,\"device_id\":\"$device_id\",\"device_type\":\"cns_box\",\"status\":\"offline\"}"
wait_for_event registration_offline
wait_for_sql "SELECT status FROM device_latest_states WHERE device_id='$device_id'" offline
mosquitto_pub "${MQTT[@]}" -q 0 -t "$topic_namespace/$device_id/telemetry/snapshot/v1" \
  -m "{\"schema_version\":3,\"device_id\":\"$device_id\",\"device_type\":\"cns_box\",\"sent_at\":\"2026-08-03T10:00:03Z\",\"telemetry\":{\"sequence\":3},\"drone_id\":{\"basic_id\":{\"id_type\":1,\"ua_type\":2}}}"
wait_for_sql "SELECT status FROM device_latest_states WHERE device_id='$device_id'" online

echo "验证 ${offline_seconds} 秒活动超时离线……"
sleep "$((offline_seconds + 2))"
wait_for_event activity_timeout
wait_for_sql "SELECT status FROM device_latest_states WHERE device_id='$device_id'" offline

echo '通过：迁移、建档、状态事件、最后值、显式/超时离线、活动恢复、未知设备拒绝和非 retained telemetry。'
echo '说明：数据库断线 degraded、恢复补写及恢复后的 retained 重放需在可控数据库代理环境中另行执行。'
