#!/usr/bin/env bash
# 在用户显式提供的独立 PostgreSQL 与 Mosquitto 上验证飞控命令路由，不管理系统服务。
set -euo pipefail

config=""
migrations=""
broker_host=""
broker_port=""
binary=""
database_url=""
source_id=""
vendor_id=""
control_timeout=""
check_only=false

usage() {
  echo "用法：$0 --config <测试配置> --migrations <迁移目录> --broker-host <地址> --broker-port <端口> --binary <程序> --database-url <测试库连接> --source-id <固定来源> --vendor-id <测试设备> [--control-timeout <秒>] [--check-only]"
}

while (($#)); do
  case "$1" in
    --config) config=${2-}; shift 2 ;;
    --migrations) migrations=${2-}; shift 2 ;;
    --broker-host) broker_host=${2-}; shift 2 ;;
    --broker-port) broker_port=${2-}; shift 2 ;;
    --binary) binary=${2-}; shift 2 ;;
    --database-url) database_url=${2-}; shift 2 ;;
    --source-id) source_id=${2-}; shift 2 ;;
    --vendor-id) vendor_id=${2-}; shift 2 ;;
    --control-timeout) control_timeout=${2-}; shift 2 ;;
    --check-only) check_only=true; shift ;;
    --help|-h) usage; exit 0 ;;
    *) echo "错误：未知参数 $1" >&2; usage >&2; exit 2 ;;
  esac
done

for value in config migrations broker_host broker_port binary source_id vendor_id; do
  [[ -n "${!value}" ]] || { echo "错误：缺少必需参数 --${value//_/-}" >&2; exit 2; }
done
for tool in mosquitto_pub mosquitto_sub jq psql; do
  command -v "$tool" >/dev/null || { echo "错误：缺少工具 $tool" >&2; exit 2; }
done
[[ -d "$migrations" && -x "$binary" ]] || {
  echo "错误：迁移目录或可执行文件不可用" >&2
  exit 2
}
[[ "$vendor_id" =~ ^[A-Za-z0-9]{20}$ ]] || { echo "错误：vendor_id 必须为 20 个 ASCII 字母或数字" >&2; exit 2; }
[[ "$source_id" =~ ^[A-Za-z0-9._-]+$ ]] || { echo "错误：source_id 格式非法" >&2; exit 2; }
if [[ -z "$control_timeout" && -r "$config" ]]; then
  control_timeout=$(jq -er '.command.control_timeout_seconds' "$config")
fi
control_timeout=${control_timeout:-30}
[[ "$control_timeout" =~ ^[1-9][0-9]*$ ]] || { echo "错误：control-timeout 必须为正整数" >&2; exit 2; }

if $check_only; then
  echo "检查完成：参数、迁移目录、程序和必需工具均可用；未连接数据库或 Broker"
  exit 0
fi
[[ -n "$database_url" ]] || { echo "错误：真实联调必须提供 --database-url" >&2; exit 2; }
[[ -r "$config" ]] || { echo "错误：真实联调配置不可读" >&2; exit 2; }

topic_namespace=$(jq -er '.mqtt.topic_namespace' "$config")
request_prefix="m4-control-local-"
run_suffix="$(date +%s)-$$-$RANDOM"
request_id="${request_prefix}${run_suffix}"
school_name="CNS_M4_LOCAL_${run_suffix}"
dcdw_label="DCDW-M4-${run_suffix: -8}"
work_dir=$(mktemp -d)
service_pid=""
source_subscriber_pid=""
device_subscriber_pid=""
device_preexisting=0
MQTT=(-h "$broker_host" -p "$broker_port")

cleanup() {
  local exit_code=$?
  trap - EXIT INT TERM
  [[ -z "$source_subscriber_pid" ]] || kill "$source_subscriber_pid" 2>/dev/null || true
  [[ -z "$device_subscriber_pid" ]] || kill "$device_subscriber_pid" 2>/dev/null || true
  [[ -z "$service_pid" ]] || kill -TERM "$service_pid" 2>/dev/null || true
  [[ -z "$service_pid" ]] || wait "$service_pid" 2>/dev/null || true
  psql "$database_url" -X -v ON_ERROR_STOP=1 -v source_id="$source_id" \
    -v request_prefix="${request_prefix}%" -c \
    "DELETE FROM commands WHERE source_id = :'source_id' AND request_id LIKE :'request_prefix';" \
    >/dev/null 2>&1 || true
  if ((device_preexisting == 0)); then
    mosquitto_pub "${MQTT[@]}" -q 2 -r -t "$topic_namespace/$vendor_id/registration" -n 2>/dev/null || true
    psql "$database_url" -X -v ON_ERROR_STOP=1 -v vendor_id="$vendor_id" \
      -v school_name="$school_name" >/dev/null 2>&1 <<'SQL' || true
DELETE FROM command_sources WHERE source_id = :'vendor_id' AND device_vendor_id = :'vendor_id';
DELETE FROM device_latest_states WHERE vendor_id = :'vendor_id';
DELETE FROM devices WHERE vendor_id = :'vendor_id';
DELETE FROM schools WHERE school_name = :'school_name'
  AND NOT EXISTS (SELECT 1 FROM devices WHERE devices.school_id = schools.school_id);
SQL
  fi
  ((exit_code == 0)) || { echo "联调失败，服务日志如下：" >&2; sed -n '1,240p' "$work_dir/service.log" >&2 || true; }
  rm -rf -- "$work_dir"
  exit "$exit_code"
}
trap cleanup EXIT INT TERM

wait_for_json() {
  local file=$1 filter=$2 description=$3 deadline=$((SECONDS + control_timeout + 10))
  while ((SECONDS < deadline)); do
    jq -e "$filter" "$file" >/dev/null 2>&1 && return 0
    sleep 0.1
  done
  echo "错误：等待${description}超时" >&2
  return 1
}

wait_for_database_status() {
  local wanted=$1 target_request=$2 deadline=$((SECONDS + control_timeout + 10)) actual=""
  while ((SECONDS < deadline)); do
    actual=$(psql "$database_url" -X -Atq -v ON_ERROR_STOP=1 -v source_id="$source_id" \
      -v request_id="$target_request" -c \
      "SELECT status FROM commands WHERE source_id = :'source_id' AND request_id = :'request_id';" 2>/dev/null || true)
    [[ "$actual" == "$wanted" ]] && return 0
    sleep 0.1
  done
  echo "错误：等待数据库状态 $wanted 超时，实际为 $actual" >&2
  return 1
}

start_service() {
  : >"$work_dir/service.log"
  "$binary" --config "$config" --migrations "$migrations" >>"$work_dir/service.log" 2>&1 &
  service_pid=$!
  sleep 1
  kill -0 "$service_pid"
}

stop_service() {
  [[ -z "$service_pid" ]] && return 0
  kill -TERM "$service_pid"
  wait "$service_pid" || true
  service_pid=""
}

publish_request() {
  local target_request=$1
  mosquitto_pub "${MQTT[@]}" -q 2 -t "$topic_namespace/sources/$source_id/control/request" \
    -m "$(jq -cn --arg request_id "$target_request" --arg vendor_id "$vendor_id" \
      '{schema_version:1,request_id:$request_id,target:{vendor_id:$vendor_id},command:"set_motor_pwm",parameters:{pwm_us:[1000,1100,1200,1300]}}')"
}

next_device_set() {
  local line_number=$1 deadline=$((SECONDS + 15))
  while ((SECONDS < deadline)); do
    if [[ $(wc -l <"$work_dir/device-set.jsonl") -ge $line_number ]]; then
      sed -n "${line_number}p" "$work_dir/device-set.jsonl"
      return 0
    fi
    sleep 0.1
  done
  echo "错误：等待第 $line_number 次设备下发超时" >&2
  return 1
}

echo "验证迁移连续执行两次……"
"$binary" --config "$config" --migrations "$migrations" --migrate-only
"$binary" --config "$config" --migrations "$migrations" --migrate-only
device_preexisting=$(psql "$database_url" -X -Atq -c "SELECT count(*) FROM devices WHERE vendor_id = '$vendor_id';")

mosquitto_pub "${MQTT[@]}" -q 2 -r -t "$topic_namespace/$vendor_id/registration" \
  -m "$(jq -cn --arg vendor_id "$vendor_id" --arg school_name "$school_name" --arg dcdw_label "$dcdw_label" \
    '{schema_version:1,vendor_id:$vendor_id,school_name:$school_name,dcdw_label:$dcdw_label,status:"online"}')"
start_service
deadline=$((SECONDS + 15))
until [[ $(psql "$database_url" -X -Atq -c "SELECT count(*) FROM devices WHERE vendor_id = '$vendor_id';") == 1 ]]; do
  ((SECONDS < deadline)) || { echo "错误：retained registration 未完成建档" >&2; exit 1; }
  sleep 0.1
done

: >"$work_dir/source-ack.jsonl"
: >"$work_dir/device-set.jsonl"
mosquitto_sub "${MQTT[@]}" -q 2 -t "$topic_namespace/sources/$source_id/control/ack" >"$work_dir/source-ack.jsonl" &
source_subscriber_pid=$!
mosquitto_sub "${MQTT[@]}" -q 2 -t "$topic_namespace/$vendor_id/control/set" >"$work_dir/device-set.jsonl" &
device_subscriber_pid=$!
sleep 1

echo "验证 dispatched、in_progress 与 accepted 回程……"
success_request="${request_id}-success"
publish_request "$success_request"
set_payload=$(next_device_set 1)
command_id=$(jq -er '.command_id' <<<"$set_payload")
jq -e 'keys == ["command","command_id","parameters"] and .command == "set_motor_pwm" and .parameters.pwm_us == [1000,1100,1200,1300]' <<<"$set_payload" >/dev/null
wait_for_json "$work_dir/source-ack.jsonl" "select(.request_id == \"$success_request\" and .status == \"dispatched\")" "来源 dispatched ACK"
mosquitto_pub "${MQTT[@]}" -q 2 -t "$topic_namespace/$vendor_id/control/ack" \
  -m "$(jq -cn --arg command_id "$command_id" '{command_id:$command_id,command:"set_motor_pwm",status:"in_progress",mavlink_command:31013,result:5,result_code:"in_progress",progress:40,result_param2:0}')"
wait_for_json "$work_dir/source-ack.jsonl" "select(.request_id == \"$success_request\" and .status == \"in_progress\")" "来源进度 ACK"
mosquitto_pub "${MQTT[@]}" -q 2 -t "$topic_namespace/$vendor_id/control/ack" \
  -m "$(jq -cn --arg command_id "$command_id" '{command_id:$command_id,command:"set_motor_pwm",status:"accepted",mavlink_command:31013,result:0,result_code:"accepted"}')"
wait_for_json "$work_dir/source-ack.jsonl" "select(.request_id == \"$success_request\" and .status == \"succeeded\")" "来源成功 ACK"
wait_for_database_status succeeded "$success_request"

echo "验证设备 rejected 与设备 timeout……"
rejected_request="${request_id}-rejected"
publish_request "$rejected_request"
rejected_id=$(jq -er '.command_id' <<<"$(next_device_set 2)")
mosquitto_pub "${MQTT[@]}" -q 2 -t "$topic_namespace/$vendor_id/control/ack" \
  -m "$(jq -cn --arg command_id "$rejected_id" '{command_id:$command_id,command:"set_motor_pwm",status:"rejected",mavlink_command:31013,result:2,result_code:"denied"}')"
wait_for_database_status failed "$rejected_request"

device_timeout_request="${request_id}-device-timeout"
publish_request "$device_timeout_request"
device_timeout_id=$(jq -er '.command_id' <<<"$(next_device_set 3)")
mosquitto_pub "${MQTT[@]}" -q 2 -t "$topic_namespace/$vendor_id/control/ack" \
  -m "$(jq -cn --arg command_id "$device_timeout_id" '{command_id:$command_id,command:"set_motor_pwm",status:"timeout",error_code:"mcu_ack_timeout"}')"
wait_for_database_status timeout "$device_timeout_request"

echo "验证服务器超时……"
server_timeout_request="${request_id}-server-timeout"
publish_request "$server_timeout_request"
next_device_set 4 >/dev/null
wait_for_database_status timeout "$server_timeout_request"

echo "验证重启后 delivery_uncertain 且不重复下发……"
restart_request="${request_id}-restart"
publish_request "$restart_request"
next_device_set 5 >/dev/null
wait_for_database_status dispatched "$restart_request"
device_set_count_before_restart=$(wc -l <"$work_dir/device-set.jsonl")
stop_service
start_service
wait_for_database_status delivery_uncertain "$restart_request"
sleep 2
device_set_count_after_restart=$(wc -l <"$work_dir/device-set.jsonl")
if [[ "$device_set_count_after_restart" != "$device_set_count_before_restart" ]]; then
  echo "错误：重启后重复下发了活动飞控命令" >&2
  exit 1
fi

echo "验收通过：迁移幂等、建档、飞控进度与终态、两类超时及重启不重发均符合预期"
