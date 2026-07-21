#include "core/command/control_policy.hpp"

#include "core/command/command_validation.hpp"

#include <array>
#include <limits>
#include <optional>

namespace cns::command {
namespace {

using Json = nlohmann::json;

ProtocolError Error(std::string code, std::string message) {
  return {std::move(code), std::move(message)};
}

std::string_view CommandText(ControlCommand command) noexcept {
  switch (command) {
    case ControlCommand::kSetMotorPwm:
      return "set_motor_pwm";
    case ControlCommand::kEmergencyStop:
      return "emergency_stop";
    case ControlCommand::kTakeoff:
      return "takeoff";
    case ControlCommand::kLand:
      return "land";
  }
  return {};
}

std::optional<ControlCommand> ParseCommandText(std::string_view text) {
  if (text == "set_motor_pwm") return ControlCommand::kSetMotorPwm;
  if (text == "emergency_stop") return ControlCommand::kEmergencyStop;
  if (text == "takeoff") return ControlCommand::kTakeoff;
  if (text == "land") return ControlCommand::kLand;
  return std::nullopt;
}

template <typename Integer>
std::optional<Integer> ReadInteger(const Json& root, std::string_view field,
                                   std::int64_t minimum,
                                   std::uint64_t maximum, bool& valid) {
  if (!root.contains(field)) return std::nullopt;
  const auto& value = root.at(field);
  if (!value.is_number_integer() && !value.is_number_unsigned()) {
    valid = false;
    return std::nullopt;
  }
  if (value.is_number_integer()) {
    const auto number = value.get<std::int64_t>();
    if (number < minimum ||
        (number >= 0 && static_cast<std::uint64_t>(number) > maximum)) {
      valid = false;
      return std::nullopt;
    }
    return static_cast<Integer>(number);
  }
  const auto number = value.get<std::uint64_t>();
  if (minimum > 0 && number < static_cast<std::uint64_t>(minimum)) {
    valid = false;
    return std::nullopt;
  }
  if (number > maximum) {
    valid = false;
    return std::nullopt;
  }
  return static_cast<Integer>(number);
}

std::optional<std::string> ReadString(const Json& root, std::string_view field,
                                      bool& valid) {
  if (!root.contains(field)) return std::nullopt;
  if (!root.at(field).is_string()) {
    valid = false;
    return std::nullopt;
  }
  return root.at(field).get<std::string>();
}

std::string_view ResultCode(std::uint8_t result) {
  switch (result) {
    case 0:
      return "accepted";
    case 1:
      return "temporarily_rejected";
    case 2:
      return "denied";
    case 3:
      return "unsupported";
    case 4:
      return "failed";
    case 5:
      return "in_progress";
    case 6:
      return "cancelled";
    default:
      return "unknown_result";
  }
}

}  // namespace

std::uint16_t ExpectedMavlinkCommand(ControlCommand command) noexcept {
  switch (command) {
    case ControlCommand::kSetMotorPwm:
      return 31013;
    case ControlCommand::kEmergencyStop:
      return 31090;
    case ControlCommand::kTakeoff:
      return 31091;
    case ControlCommand::kLand:
      return 31092;
  }
  return 0;
}

nlohmann::json BuildDeviceControlSet(std::string_view command_id,
                                     ControlCommand command,
                                     const ControlParameters& parameters) {
  Json values = Json::object();
  if (parameters.pwm_us) values["pwm_us"] = *parameters.pwm_us;
  return {{"command_id", command_id},
          {"command", CommandText(command)},
          {"parameters", std::move(values)}};
}

std::expected<DeviceControlAck, ProtocolError> ParseDeviceControlAck(
    std::string_view payload) {
  Json root;
  try {
    root = Json::parse(payload);
  } catch (const Json::exception&) {
    return std::unexpected(Error("invalid_json", "设备飞控ACK不是合法JSON"));
  }
  if (!root.is_object()) {
    return std::unexpected(Error("invalid_json", "设备飞控ACK必须是JSON对象"));
  }
  if (!root.contains("command_id") || !root.at("command_id").is_string() ||
      !validation::IsUuidV4Shape(
          root.at("command_id").get_ref<const std::string&>())) {
    return std::unexpected(Error("invalid_command_id", "command_id缺失或非法"));
  }
  if (!root.contains("command") || !root.at("command").is_string()) {
    return std::unexpected(Error("invalid_ack", "command缺失或非法"));
  }
  const auto command =
      ParseCommandText(root.at("command").get_ref<const std::string&>());
  if (!command) return std::unexpected(Error("invalid_ack", "command不受支持"));
  if (!root.contains("status") || !root.at("status").is_string()) {
    return std::unexpected(Error("invalid_ack", "status缺失或非法"));
  }
  const auto status = root.at("status").get<std::string>();
  if (status != "accepted" && status != "in_progress" && status != "rejected" &&
      status != "timeout") {
    return std::unexpected(Error("invalid_ack", "设备飞控ACK状态非法"));
  }

  bool valid = true;
  auto mavlink_command = ReadInteger<std::uint16_t>(
      root, "mavlink_command", 0, std::numeric_limits<std::uint16_t>::max(), valid);
  auto result = ReadInteger<std::uint8_t>(root, "result", 0, 255, valid);
  auto result_code = ReadString(root, "result_code", valid);
  auto progress = ReadInteger<std::uint8_t>(root, "progress", 0, 255, valid);
  auto result_param2 = ReadInteger<std::int32_t>(
      root, "result_param2", std::numeric_limits<std::int32_t>::min(),
      static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()), valid);
  auto error_code = ReadString(root, "error_code", valid);
  if (!valid || (root.contains("message") && !root.at("message").is_string())) {
    return std::unexpected(Error("invalid_ack", "设备飞控ACK字段类型非法"));
  }
  if (mavlink_command && *mavlink_command != ExpectedMavlinkCommand(*command)) {
    return std::unexpected(Error("invalid_ack", "mavlink_command与command不匹配"));
  }
  if (error_code && !validation::IsValidErrorCode(*error_code)) {
    return std::unexpected(Error("invalid_ack", "error_code非法"));
  }

  if (status == "accepted") {
    if (!mavlink_command || result != 0 || result_code != "accepted" || progress ||
        result_param2 || error_code) {
      return std::unexpected(Error("invalid_ack", "accepted ACK字段组合非法"));
    }
  } else if (status == "in_progress") {
    if (!mavlink_command || error_code) {
      return std::unexpected(Error("invalid_ack", "in_progress ACK字段组合非法"));
    }
    if (result_code == "pending") {
      if (result || progress || result_param2) {
        return std::unexpected(Error("invalid_ack", "pending ACK字段组合非法"));
      }
    } else if (result != 5 || result_code != "in_progress" || !progress ||
               !result_param2 || (*progress > 100 && *progress != 255)) {
      return std::unexpected(Error("invalid_ack", "MAVLink进度ACK字段组合非法"));
    }
  } else if (status == "rejected") {
    const bool mavlink_rejected = mavlink_command || result || result_code;
    if (mavlink_rejected) {
      if (!mavlink_command || !result || !result_code || error_code || progress ||
          result_param2 || *result == 0 || *result == 5 ||
          *result_code != ResultCode(*result)) {
        return std::unexpected(Error("invalid_ack", "MAVLink拒绝ACK字段组合非法"));
      }
    } else if (!error_code) {
      return std::unexpected(Error("invalid_ack", "本地拒绝ACK缺少error_code"));
    }
  } else if (!error_code || result || result_code || progress || result_param2) {
    return std::unexpected(Error("invalid_ack", "timeout ACK字段组合非法"));
  }

  return DeviceControlAck{root.at("command_id").get<std::string>(),
                          *command,
                          status,
                          mavlink_command,
                          result,
                          result_code,
                          progress,
                          result_param2,
                          error_code,
                          std::move(root)};
}

}  // namespace cns::command
