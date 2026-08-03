#!/usr/bin/env bash
# 非破坏性验证固定来源经 route_service 路由配置命令并收到设备与来源 ACK。
set -euo pipefail

config=""
migrations=""
broker_host=""
broker_port=""
binary=""
database_url=""
source_id=""
device_id=""
check_only=false

while (($#)); do
  case "$1" in
    --config) config="$2"; shift 2 ;;
    --migrations) migrations="$2"; shift 2 ;;
    --broker-host) broker_host="$2"; shift 2 ;;
    --broker-port) broker_port="$2"; shift 2 ;;
    --binary) binary="$2"; shift 2 ;;
    --database-url) database_url="$2"; shift 2 ;;
    --source-id) source_id="$2"; shift 2 ;;
    --device-id) device_id="$2"; shift 2 ;;
    --check-only) check_only=true; shift ;;
    *) echo "错误：未知参数 $1" >&2; exit 2 ;;
  esac
done

for value in config migrations broker_host broker_port binary source_id device_id; do
  if [[ -z "${!value}" ]]; then
    echo "错误：缺少必需参数 --${value//_/-}" >&2
    exit 2
  fi
done
for tool in mosquitto_pub mosquitto_sub jq; do
  command -v "$tool" >/dev/null || { echo "错误：缺少工具 $tool" >&2; exit 2; }
done
[[ -r "$config" && -d "$migrations" && -x "$binary" ]] || {
  echo "错误：配置、迁移目录或可执行文件不可用" >&2; exit 2;
}
if $check_only; then
  echo "检查完成：参数、文件和 MQTT 客户端均可用"
  exit 0
fi
[[ -n "$database_url" ]] || { echo "错误：真实联调必须提供 --database-url" >&2; exit 2; }
command -v psql >/dev/null || { echo "错误：缺少工具 psql" >&2; exit 2; }

request_id="m3-local-$(date +%s)-$$"
work_dir="$(mktemp -d)"
service_pid=""
subscriber_pid=""
device_subscriber_pid=""
cleanup() {
  [[ -z "$subscriber_pid" ]] || kill "$subscriber_pid" 2>/dev/null || true
  [[ -z "$device_subscriber_pid" ]] || kill "$device_subscriber_pid" 2>/dev/null || true
  [[ -z "$service_pid" ]] || kill -TERM "$service_pid" 2>/dev/null || true
  [[ -z "$service_pid" ]] || wait "$service_pid" 2>/dev/null || true
  psql "$database_url" -v ON_ERROR_STOP=1 -v source_id="$source_id" \
    -v request_id="$request_id" -c \
    "DELETE FROM commands WHERE source_id = :'source_id' AND request_id = :'request_id';" \
    >/dev/null 2>&1 || true
  rm -rf -- "$work_dir"
}
trap cleanup EXIT

"$binary" --config "$config" --migrations "$migrations" >"$work_dir/service.log" 2>&1 &
service_pid=$!
sleep 1
mosquitto_sub -h "$broker_host" -p "$broker_port" -q 2 \
  -t "cns_rpi/sources/$source_id/config/ack" >"$work_dir/source-ack.jsonl" &
subscriber_pid=$!
mosquitto_sub -h "$broker_host" -p "$broker_port" -q 2 -C 1 -W 20 \
  -t "cns_rpi/$device_id/config/set" >"$work_dir/device-set.json" &
device_subscriber_pid=$!
sleep 1

mosquitto_pub -h "$broker_host" -p "$broker_port" -q 2 \
  -t "cns_rpi/sources/$source_id/config/request" -m "$(jq -cn \
    --arg request_id "$request_id" --arg device_id "$device_id" \
    '{schema_version:1,request_id:$request_id,target:{device_id:$device_id},parameters:{telemetry_publish_interval_ms:2000}}')"

wait "$device_subscriber_pid"
device_subscriber_pid=""
device_set="$(<"$work_dir/device-set.json")"
command_id="$(jq -er '.command_id' <<<"$device_set")"
mosquitto_pub -h "$broker_host" -p "$broker_port" -q 2 \
  -t "cns_rpi/$device_id/config/ack" -m "$(jq -cn --arg command_id "$command_id" \
    '{command_id:$command_id,status:"applied",restart_required:false}')"
deadline=$((SECONDS + 20))
while ! jq -e 'select(.status == "succeeded")' \
    "$work_dir/source-ack.jsonl" >/dev/null 2>&1; do
  ((SECONDS < deadline)) || { echo "错误：等待来源 succeeded ACK 超时" >&2; exit 1; }
  sleep 0.2
done
kill "$subscriber_pid" 2>/dev/null || true
wait "$subscriber_pid" 2>/dev/null || true
subscriber_pid=""
echo "验收通过：配置命令已经过 route_service 路由并完成设备 ACK 回程"
