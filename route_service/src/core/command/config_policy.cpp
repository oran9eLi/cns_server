/**
 * @file config_policy.cpp
 * @brief 生成设备配置命令并严格校验设备 ACK 的关联字段。
 */
#include "core/command/config_policy.hpp"
#include "core/command/command_validation.hpp"

#include <algorithm>
#include <array>

namespace cns::command {
namespace {

using Json = nlohmann::json;

ProtocolError Error(std::string code, std::string message) {
  return {std::move(code), std::move(message)};
}

}  // namespace

nlohmann::json BuildDeviceConfigSet(std::string_view command_id,
                                    const ConfigParameters& parameters) {
  Json values = Json::object();
  if (parameters.telemetry_publish_interval_ms) {
    values["telemetry_publish_interval_ms"] =
        *parameters.telemetry_publish_interval_ms;
  }
  if (parameters.heartbeat_interval_ms) {
    values["heartbeat_interval_ms"] = *parameters.heartbeat_interval_ms;
  }
  if (parameters.mqtt_reconnect_delay_s) {
    values["mqtt_reconnect_delay_s"] = *parameters.mqtt_reconnect_delay_s;
  }
  if (parameters.mqtt_reconnect_delay_max_s) {
    values["mqtt_reconnect_delay_max_s"] =
        *parameters.mqtt_reconnect_delay_max_s;
  }
  return {{"command_id", command_id}, {"parameters", std::move(values)}};
}

std::expected<DeviceConfigAck, ProtocolError> ParseDeviceConfigAck(
    std::string_view payload) {
  Json root;
  try {
    root = Json::parse(payload);
  } catch (const Json::exception&) {
    return std::unexpected(Error("invalid_json", "设备配置ACK不是合法JSON"));
  }
  if (!root.is_object()) {
    return std::unexpected(Error("invalid_json", "设备配置ACK必须是JSON对象"));
  }
  if (!root.contains("command_id") || !root.at("command_id").is_string() ||
      !validation::IsUuidV4Shape(
          root.at("command_id").get_ref<const std::string&>())) {
    return std::unexpected(Error("invalid_command_id", "command_id缺失或非法"));
  }
  if (!root.contains("status") || !root.at("status").is_string()) {
    return std::unexpected(Error("invalid_ack", "status缺失或非法"));
  }
  const auto status = root.at("status").get<std::string>();
  if (status != "applied" && status != "already_applied" && status != "rejected") {
    return std::unexpected(Error("invalid_ack", "设备配置ACK状态非法"));
  }
  if (!root.contains("restart_required") ||
      !root.at("restart_required").is_boolean()) {
    return std::unexpected(Error("invalid_ack", "restart_required缺失或非法"));
  }
  std::optional<std::string> error_code;
  if (status == "rejected") {
    if (!root.contains("error_code") || !root.at("error_code").is_string() ||
        !validation::IsValidErrorCode(
            root.at("error_code").get_ref<const std::string&>())) {
      return std::unexpected(Error("invalid_ack", "rejected ACK缺少合法error_code"));
    }
    error_code = root.at("error_code").get<std::string>();
  } else if (root.contains("error_code") && !root.at("error_code").is_null()) {
    return std::unexpected(Error("invalid_ack", "成功ACK不得包含error_code"));
  }
  if (root.contains("message") && !root.at("message").is_string()) {
    return std::unexpected(Error("invalid_ack", "message类型非法"));
  }
  return DeviceConfigAck{root.at("command_id").get<std::string>(), status,
                         root.at("restart_required").get<bool>(), error_code,
                         std::move(root)};
}

}  // namespace cns::command
