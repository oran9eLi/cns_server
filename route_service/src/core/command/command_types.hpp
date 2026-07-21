/**
 * @file command_types.hpp
 * @brief 配置与飞控可复用的命令协议基础类型。
 *
 * 本层不依赖 MQTT、PostgreSQL 或运行时线程。
 */
#pragma once

#include <cstdint>
#include <array>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include <nlohmann/json.hpp>

namespace cns::command {

enum class SourceKind { kDevice, kHostApp, kControlCenter };
enum class CommandType { kConfig, kControl };
enum class CommandStatus {
  kPending,
  kDispatched,
  kInProgress,
  kSucceeded,
  kFailed,
  kTimeout,
  kDeliveryUncertain
};

inline std::string_view ToString(CommandType type) noexcept {
  switch (type) {
    case CommandType::kConfig:
      return "config";
    case CommandType::kControl:
      return "control";
  }
  return {};
}

inline std::expected<CommandType, std::string> ParseCommandType(
    std::string_view text) {
  if (text == "config") return CommandType::kConfig;
  if (text == "control") return CommandType::kControl;
  return std::unexpected("未知命令类型");
}

struct DeviceLabelTarget {
  std::string dcdw_label;
};

struct SchoolLabelTarget {
  std::string school_name;
  std::string dcdw_label;
};

struct VendorTarget {
  std::string vendor_id;
};

using RequestTarget =
    std::variant<DeviceLabelTarget, SchoolLabelTarget, VendorTarget>;

struct ConfigParameters {
  std::optional<std::uint32_t> telemetry_publish_interval_ms;
  std::optional<std::uint32_t> heartbeat_interval_ms;
  std::optional<std::uint32_t> mqtt_reconnect_delay_s;
  std::optional<std::uint32_t> mqtt_reconnect_delay_max_s;

  bool operator==(const ConfigParameters&) const = default;
};

struct SourceConfigRequest {
  std::string request_id;
  RequestTarget target;
  ConfigParameters parameters;
  nlohmann::json comparison_payload;
};

enum class ControlCommand { kSetMotorPwm, kEmergencyStop, kTakeoff, kLand };

struct ControlParameters {
  std::optional<std::array<std::uint16_t, 4>> pwm_us;

  bool operator==(const ControlParameters&) const = default;
};

struct SourceControlRequest {
  std::string request_id;
  RequestTarget target;
  ControlCommand command;
  ControlParameters parameters;
  nlohmann::json comparison_payload;
};

struct DeviceControlAck {
  std::string command_id;
  ControlCommand command;
  std::string business_status;
  std::optional<std::uint16_t> mavlink_command;
  std::optional<std::uint8_t> result;
  std::optional<std::string> result_code;
  std::optional<std::uint8_t> progress;
  std::optional<std::int32_t> result_param2;
  std::optional<std::string> error_code;
  nlohmann::json raw;
};

struct DeviceConfigAck {
  std::string command_id;
  std::string business_status;
  bool restart_required;
  std::optional<std::string> error_code;
  nlohmann::json raw;
};

struct ProtocolError {
  std::string code;
  std::string message;
};

struct RejectedSourceRequest {
  std::optional<std::string> request_id;
  std::optional<nlohmann::json> comparison_payload;
  ProtocolError error;
};

using ParsedSourceRequest =
    std::variant<SourceConfigRequest, SourceControlRequest, RejectedSourceRequest>;
using SourceRequestParseResult = ParsedSourceRequest;

}  // namespace cns::command
