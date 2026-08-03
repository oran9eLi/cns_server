/**
 * @file source_request.cpp
 * @brief 严格解析来源请求并保留可建立幂等键的 JSONB 比较对象。
 */
#include "core/command/source_request.hpp"

#include "core/mqtt_topic/device_topic.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <optional>
#include <ranges>

namespace cns::command {
namespace {

using Json = nlohmann::json;

ProtocolError Error(std::string code, std::string message) {
  return {std::move(code), std::move(message)};
}

bool IsValidSourceId(std::string_view value) {
  return !value.empty() && value.size() <= 64 &&
         std::ranges::all_of(value, [](unsigned char character) {
           return std::isalnum(character) != 0 || character == '.' ||
                  character == '_' || character == '-';
         });
}

bool IsValidRequestId(std::string_view value) {
  return !value.empty() && value.size() <= 128 &&
         std::ranges::all_of(value, [](unsigned char character) {
           return std::isalnum(character) != 0 || character == '.' ||
                  character == '_' || character == '-' || character == ':';
         });
}

bool IsNonEmptyStringWithin(const Json& object, std::string_view key,
                            std::size_t maximum) {
  if (!object.contains(key) || !object.at(key).is_string()) return false;
  const auto& value = object.at(key).get_ref<const std::string&>();
  return !value.empty() && value.size() <= maximum;
}

std::optional<std::uint32_t> ReadUnsignedInRange(const Json& object,
                                                 std::string_view key,
                                                 std::uint32_t minimum,
                                                 std::uint32_t maximum,
                                                 bool& valid) {
  if (!object.contains(key)) return std::nullopt;
  const auto& value = object.at(key);
  if ((!value.is_number_integer() && !value.is_number_unsigned()) ||
      (value.is_number_integer() && value.get<std::int64_t>() < 0)) {
    valid = false;
    return std::nullopt;
  }
  const auto number = value.get<std::uint64_t>();
  if (number < minimum || number > maximum) {
    valid = false;
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(number);
}

std::expected<ConfigParameters, ProtocolError> ParseParameters(const Json& value) {
  if (!value.is_object() || value.empty()) {
    return std::unexpected(Error("invalid_parameters", "parameters必须是非空对象"));
  }
  constexpr std::array<std::string_view, 4> allowed{
      "telemetry_publish_interval_ms", "heartbeat_interval_ms",
      "mqtt_reconnect_delay_s", "mqtt_reconnect_delay_max_s"};
  for (const auto& [field, unused] : value.items()) {
    (void)unused;
    if (std::ranges::find(allowed, field) == allowed.end()) {
      return std::unexpected(Error("invalid_parameters", "parameters包含未知字段"));
    }
  }
  bool valid = true;
  ConfigParameters parameters{
      .telemetry_publish_interval_ms = ReadUnsignedInRange(
          value, "telemetry_publish_interval_ms", 100, 60000, valid),
      .heartbeat_interval_ms =
          ReadUnsignedInRange(value, "heartbeat_interval_ms", 100, 60000, valid),
      .mqtt_reconnect_delay_s =
          ReadUnsignedInRange(value, "mqtt_reconnect_delay_s", 1, 3600, valid),
      .mqtt_reconnect_delay_max_s = ReadUnsignedInRange(
          value, "mqtt_reconnect_delay_max_s", 1, 3600, valid)};
  if (!valid) {
    return std::unexpected(Error("invalid_parameters", "配置参数类型或范围非法"));
  }
  if (parameters.mqtt_reconnect_delay_s && parameters.mqtt_reconnect_delay_max_s &&
      *parameters.mqtt_reconnect_delay_s >
          *parameters.mqtt_reconnect_delay_max_s) {
    return std::unexpected(
        Error("invalid_parameters", "MQTT重连初值不得大于最大值"));
  }
  return parameters;
}

std::expected<RequestTarget, ProtocolError> ParseTarget(const Json& value,
                                                        SourceKind kind) {
  if (!value.is_object()) {
    return std::unexpected(Error("invalid_target", "target必须是对象"));
  }
  if (kind == SourceKind::kDevice) {
    if (value.size() != 1 ||
        !IsNonEmptyStringWithin(value, "dcdw_label", 64)) {
      return std::unexpected(
          Error("invalid_target", "设备来源只能按同校角色号寻址"));
    }
    return DeviceLabelTarget{value.at("dcdw_label").get<std::string>()};
  }
  if (value.size() == 1 && value.contains("device_id") &&
      value.at("device_id").is_string() &&
      mqtt_topic::IsValidDeviceId(value.at("device_id").get_ref<const std::string&>())) {
    return DeviceTarget{value.at("device_id").get<std::string>()};
  }
  if (value.size() == 2 && IsNonEmptyStringWithin(value, "school_name", 128) &&
      IsNonEmptyStringWithin(value, "dcdw_label", 64)) {
    return SchoolLabelTarget{value.at("school_name").get<std::string>(),
                             value.at("dcdw_label").get<std::string>()};
  }
  return std::unexpected(Error("invalid_target", "非设备来源目标形式非法或混用"));
}

RejectedSourceRequest Reject(std::optional<std::string> request_id,
                             std::optional<Json> comparison_payload,
                             ProtocolError error) {
  return {std::move(request_id), std::move(comparison_payload), std::move(error)};
}

std::expected<std::string, ProtocolError> ParseSourceTopic(
    std::string_view topic_namespace, std::string_view topic,
    std::string_view suffix, std::string_view description) {
  const std::string prefix = std::string{topic_namespace} + "/sources/";
  if (!topic.starts_with(prefix) || !topic.ends_with(suffix) ||
      topic.size() <= prefix.size() + suffix.size()) {
    return std::unexpected(
        Error("invalid_topic", std::string{description} + "topic格式无效"));
  }
  const auto source_id =
      topic.substr(prefix.size(), topic.size() - prefix.size() - suffix.size());
  if (!IsValidSourceId(source_id) || source_id.find('/') != std::string_view::npos) {
    return std::unexpected(Error("invalid_topic", "source_id无效"));
  }
  return std::string{source_id};
}

std::expected<std::string, ProtocolError> ParseDeviceAckTopic(
    std::string_view topic_namespace, std::string_view topic,
    std::string_view suffix, std::string_view description) {
  const std::string prefix = std::string{topic_namespace} + "/";
  if (!topic.starts_with(prefix) || !topic.ends_with(suffix) ||
      topic.size() <= prefix.size() + suffix.size()) {
    return std::unexpected(
        Error("invalid_topic", std::string{description} + " topic格式无效"));
  }
  const auto device_id =
      topic.substr(prefix.size(), topic.size() - prefix.size() - suffix.size());
  if (!mqtt_topic::IsValidDeviceId(device_id)) {
    return std::unexpected(Error("invalid_topic", "device_id无效"));
  }
  return std::string{device_id};
}

std::expected<ControlCommand, ProtocolError> ParseControlCommand(
    const Json& root) {
  if (!root.contains("command") || !root.at("command").is_string()) {
    return std::unexpected(Error("unsupported_command", "command缺失或非法"));
  }
  const auto& command = root.at("command").get_ref<const std::string&>();
  if (command == "set_motor_pwm") return ControlCommand::kSetMotorPwm;
  if (command == "emergency_stop") return ControlCommand::kEmergencyStop;
  if (command == "takeoff") return ControlCommand::kTakeoff;
  if (command == "land") return ControlCommand::kLand;
  return std::unexpected(Error("unsupported_command", "不支持的飞控命令"));
}

std::expected<ControlParameters, ProtocolError> ParseControlParameters(
    const Json& value, ControlCommand command) {
  if (!value.is_object()) {
    return std::unexpected(Error("invalid_parameters", "parameters必须是对象"));
  }
  if (command != ControlCommand::kSetMotorPwm) {
    if (!value.empty()) {
      return std::unexpected(
          Error("invalid_parameters", "该飞控命令不接受参数"));
    }
    return ControlParameters{};
  }
  if (value.size() != 1 || !value.contains("pwm_us") ||
      !value.at("pwm_us").is_array() || value.at("pwm_us").size() != 4) {
    return std::unexpected(
        Error("invalid_parameters", "set_motor_pwm只接受四路pwm_us"));
  }
  std::array<std::uint16_t, 4> pwm{};
  for (std::size_t index = 0; index < pwm.size(); ++index) {
    const auto& item = value.at("pwm_us").at(index);
    if ((!item.is_number_integer() && !item.is_number_unsigned()) ||
        (item.is_number_integer() && item.get<std::int64_t>() < 0)) {
      return std::unexpected(
          Error("invalid_parameters", "PWM必须是1000到2000的整数"));
    }
    const auto number = item.get<std::uint64_t>();
    if (number < 1000 || number > 2000) {
      return std::unexpected(
          Error("invalid_parameters", "PWM必须是1000到2000的整数"));
    }
    pwm[index] = static_cast<std::uint16_t>(number);
  }
  return ControlParameters{pwm};
}

}  // namespace

std::expected<std::string, ProtocolError> ParseSourceRequestTopic(
    std::string_view topic_namespace, std::string_view topic) {
  return ParseSourceTopic(topic_namespace, topic, "/config/request",
                          "来源配置请求");
}

std::expected<std::string, ProtocolError> ParseDeviceConfigAckTopic(
    std::string_view topic_namespace, std::string_view topic) {
  return ParseDeviceAckTopic(topic_namespace, topic, "/config/ack", "设备配置ACK");
}

std::expected<std::string, ProtocolError> ParseSourceControlRequestTopic(
    std::string_view topic_namespace, std::string_view topic) {
  return ParseSourceTopic(topic_namespace, topic, "/control/request",
                          "来源飞控请求");
}

std::expected<std::string, ProtocolError> ParseDeviceControlAckTopic(
    std::string_view topic_namespace, std::string_view topic) {
  return ParseDeviceAckTopic(topic_namespace, topic, "/control/ack", "设备飞控ACK");
}

SourceRequestParseResult ParseSourceConfigRequest(std::string_view payload,
                                                  SourceKind kind) {
  Json root;
  try {
    root = Json::parse(payload);
  } catch (const Json::exception&) {
    return Reject(std::nullopt, std::nullopt,
                  Error("invalid_json", "来源请求不是合法JSON"));
  }
  if (!root.is_object()) {
    return Reject(std::nullopt, std::nullopt,
                  Error("invalid_json", "来源请求必须是JSON对象"));
  }

  std::optional<std::string> request_id;
  if (root.contains("request_id") && root.at("request_id").is_string() &&
      IsValidRequestId(root.at("request_id").get_ref<const std::string&>())) {
    request_id = root.at("request_id").get<std::string>();
  }
  const std::optional<Json> comparison_payload{root};
  if (!request_id) {
    return Reject(std::nullopt, comparison_payload,
                  Error("invalid_request_id", "request_id缺失或非法"));
  }

  constexpr std::array<std::string_view, 4> allowed{
      "schema_version", "request_id", "target", "parameters"};
  for (const auto& [field, unused] : root.items()) {
    (void)unused;
    if (std::ranges::find(allowed, field) == allowed.end()) {
      return Reject(request_id, comparison_payload,
                    Error("invalid_json", "来源请求包含未知字段"));
    }
  }
  if (!root.contains("schema_version") || !root.at("schema_version").is_number_integer() ||
      root.at("schema_version").get<std::int64_t>() != 1) {
    return Reject(request_id, comparison_payload,
                  Error("unsupported_schema_version", "只支持schema_version=1"));
  }
  if (!root.contains("target")) {
    return Reject(request_id, comparison_payload,
                  Error("invalid_target", "缺少target"));
  }
  const auto target = ParseTarget(root.at("target"), kind);
  if (!target) return Reject(request_id, comparison_payload, target.error());
  if (!root.contains("parameters")) {
    return Reject(request_id, comparison_payload,
                  Error("invalid_parameters", "缺少parameters"));
  }
  const auto parameters = ParseParameters(root.at("parameters"));
  if (!parameters) return Reject(request_id, comparison_payload, parameters.error());
  return SourceConfigRequest{*request_id, *target, *parameters, root};
}

ParsedSourceRequest ParseSourceControlRequest(std::string_view payload,
                                              SourceKind kind) {
  Json root;
  try {
    root = Json::parse(payload);
  } catch (const Json::exception&) {
    return Reject(std::nullopt, std::nullopt,
                  Error("invalid_json", "来源请求不是合法JSON"));
  }
  if (!root.is_object()) {
    return Reject(std::nullopt, std::nullopt,
                  Error("invalid_json", "来源请求必须是JSON对象"));
  }
  std::optional<std::string> request_id;
  if (root.contains("request_id") && root.at("request_id").is_string() &&
      IsValidRequestId(root.at("request_id").get_ref<const std::string&>())) {
    request_id = root.at("request_id").get<std::string>();
  }
  const std::optional<Json> comparison_payload{root};
  if (!request_id) {
    return Reject(std::nullopt, comparison_payload,
                  Error("invalid_request_id", "request_id缺失或非法"));
  }
  constexpr std::array<std::string_view, 5> allowed{
      "schema_version", "request_id", "target", "command", "parameters"};
  for (const auto& [field, unused] : root.items()) {
    (void)unused;
    if (std::ranges::find(allowed, field) == allowed.end()) {
      return Reject(request_id, comparison_payload,
                    Error("invalid_json", "来源请求包含未知字段"));
    }
  }
  if (!root.contains("schema_version") ||
      !root.at("schema_version").is_number_integer() ||
      root.at("schema_version").get<std::int64_t>() != 1) {
    return Reject(request_id, comparison_payload,
                  Error("unsupported_schema_version", "只支持schema_version=1"));
  }
  if (!root.contains("target")) {
    return Reject(request_id, comparison_payload,
                  Error("invalid_target", "缺少target"));
  }
  const auto target = ParseTarget(root.at("target"), kind);
  if (!target) return Reject(request_id, comparison_payload, target.error());
  const auto command = ParseControlCommand(root);
  if (!command) return Reject(request_id, comparison_payload, command.error());
  if (!root.contains("parameters")) {
    return Reject(request_id, comparison_payload,
                  Error("invalid_parameters", "缺少parameters"));
  }
  const auto parameters = ParseControlParameters(root.at("parameters"), *command);
  if (!parameters) return Reject(request_id, comparison_payload, parameters.error());
  return SourceControlRequest{*request_id, *target, *command, *parameters, root};
}

}  // namespace cns::command
