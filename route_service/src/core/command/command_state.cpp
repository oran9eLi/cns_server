#include "core/command/command_state.hpp"

#include <random>

#include "core/protocol/time_format.hpp"

namespace cns::command {
namespace {

const char* StatusText(CommandStatus status) {
  switch (status) {
    case CommandStatus::kPending:
      return "pending";
    case CommandStatus::kDispatched:
      return "dispatched";
    case CommandStatus::kInProgress:
      return "in_progress";
    case CommandStatus::kSucceeded:
      return "succeeded";
    case CommandStatus::kFailed:
      return "failed";
    case CommandStatus::kTimeout:
      return "timeout";
    case CommandStatus::kDeliveryUncertain:
      return "delivery_uncertain";
  }
  return "";
}

nlohmann::json BaseAck(nlohmann::json request_id, nlohmann::json command_id,
                       CommandType command_type, CommandStatus status,
                       TimePoint occurred_at) {
  return {{"schema_version", 1},
          {"request_id", std::move(request_id)},
          {"command_id", std::move(command_id)},
          {"command_type", ToString(command_type)},
          {"status", StatusText(status)},
          {"business_status", nullptr},
          {"occurred_at", protocol::FormatUtcRfc3339Millis(occurred_at)}};
}

}  // namespace

bool IsTerminal(CommandStatus status) noexcept {
  return status == CommandStatus::kSucceeded || status == CommandStatus::kFailed ||
         status == CommandStatus::kTimeout ||
         status == CommandStatus::kDeliveryUncertain;
}

bool CanTransition(CommandStatus from, CommandStatus to) noexcept {
  if (from == CommandStatus::kPending) {
    return to == CommandStatus::kDispatched || to == CommandStatus::kFailed ||
           to == CommandStatus::kTimeout ||
           to == CommandStatus::kDeliveryUncertain;
  }
  if (from == CommandStatus::kDispatched) {
    return to == CommandStatus::kInProgress ||
           to == CommandStatus::kSucceeded || to == CommandStatus::kFailed ||
           to == CommandStatus::kTimeout ||
           to == CommandStatus::kDeliveryUncertain;
  }
  if (from == CommandStatus::kInProgress) {
    return to == CommandStatus::kInProgress ||
           to == CommandStatus::kSucceeded || to == CommandStatus::kFailed ||
           to == CommandStatus::kTimeout ||
           to == CommandStatus::kDeliveryUncertain;
  }
  return false;
}

IdempotencyResult CompareRequest(const CommandRecord& existing,
                                 const nlohmann::json& comparison_payload) {
  return existing.request_payload == comparison_payload
             ? IdempotencyResult::kSame
             : IdempotencyResult::kConflict;
}

nlohmann::json BuildSourceAck(const CommandRecord& command,
                              const std::optional<ResolvedTarget>& target,
                              TimePoint occurred_at) {
  auto ack = BaseAck(command.request_id, command.command_id,
                     command.command_type, command.status, occurred_at);
  if (target) {
    ack["target"] = {{"device_id", target->device_id},
                     {"school_name", target->school_name},
                     {"dcdw_label", target->dcdw_label
                                          ? nlohmann::json(*target->dcdw_label)
                                          : nlohmann::json(nullptr)}};
  }
  if (command.command_type == CommandType::kControl &&
      command.status == CommandStatus::kDeliveryUncertain) {
    ack["error"] = {
        {"code", "control_delivery_uncertain"},
        {"message", "服务恢复后无法确认飞控命令是否已经执行"}};
  } else if (command.error_code) {
    ack["error"] = {{"code", *command.error_code},
                    {"message", command.error_message
                                    ? nlohmann::json(*command.error_message)
                                    : nlohmann::json(nullptr)}};
  }
  if (command.device_ack) {
    const auto& device_ack = *command.device_ack;
    if (const auto status = device_ack.find("status");
        status != device_ack.end() && status->is_string()) {
      ack["business_status"] = *status;
    }
    if (command.command_type == CommandType::kConfig) {
      ack["device"] = {
          {"restart_required", device_ack.value("restart_required", false)},
          {"error_code", device_ack.contains("error_code")
                             ? device_ack["error_code"]
                             : nlohmann::json(nullptr)}};
    } else {
      auto device = nlohmann::json::object();
      for (const auto field : {"command", "mavlink_command", "result",
                               "result_code", "progress", "result_param2",
                               "error_code"}) {
        if (device_ack.contains(field)) {
          device[field] = device_ack[field];
        }
      }
      ack["device"] = std::move(device);
    }
  }
  return ack;
}

nlohmann::json BuildPrePersistenceRejection(
    std::optional<std::string_view> request_id, ProtocolError error,
    TimePoint occurred_at, CommandType command_type) {
  auto ack = BaseAck(request_id ? nlohmann::json(*request_id)
                                : nlohmann::json(nullptr),
                     nullptr, command_type, CommandStatus::kFailed,
                     occurred_at);
  ack["error"] = {{"code", std::move(error.code)},
                  {"message", std::move(error.message)}};
  return ack;
}

std::string FormatUuidV4(std::array<std::uint8_t, 16> random_bytes) {
  random_bytes[6] = static_cast<std::uint8_t>((random_bytes[6] & 0x0fU) | 0x40U);
  random_bytes[8] = static_cast<std::uint8_t>((random_bytes[8] & 0x3fU) | 0x80U);
  constexpr char kHex[] = "0123456789abcdef";
  std::string output;
  output.reserve(36);
  for (std::size_t index = 0; index < random_bytes.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) {
      output.push_back('-');
    }
    output.push_back(kHex[random_bytes[index] >> 4U]);
    output.push_back(kHex[random_bytes[index] & 0x0fU]);
  }
  return output;
}

std::string GenerateUuidV4() {
  std::random_device random;
  std::array<std::uint8_t, 16> bytes{};
  for (auto& byte : bytes) {
    byte = static_cast<std::uint8_t>(random());
  }
  return FormatUuidV4(bytes);
}

}  // namespace cns::command
