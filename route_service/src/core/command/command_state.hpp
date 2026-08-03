#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "core/command/command_router.hpp"

namespace cns::command {

using TimePoint = std::chrono::system_clock::time_point;

struct CommandRecord {
  std::string command_id;
  CommandType command_type{CommandType::kConfig};
  std::string source_id;
  std::string request_id;
  std::optional<std::string> target_device_id;
  nlohmann::json request_payload;
  CommandStatus status;
  std::optional<std::string> error_code;
  std::optional<std::string> error_message;
  std::optional<nlohmann::json> device_ack;
  TimePoint created_at;
  std::optional<TimePoint> dispatched_at;
  TimePoint updated_at;
  std::optional<TimePoint> completed_at;
};

struct CommandUpdate {
  std::optional<std::string> error_code;
  std::optional<std::string> error_message;
  std::optional<nlohmann::json> device_ack;
  std::optional<TimePoint> dispatched_at;
  std::optional<TimePoint> completed_at;
  TimePoint updated_at;
};

bool IsTerminal(CommandStatus status) noexcept;
bool CanTransition(CommandStatus from, CommandStatus to) noexcept;

enum class IdempotencyResult { kSame, kConflict };
IdempotencyResult CompareRequest(const CommandRecord& existing,
                                 const nlohmann::json& comparison_payload);

nlohmann::json BuildSourceAck(const CommandRecord& command,
                              const std::optional<ResolvedTarget>& target,
                              TimePoint occurred_at);
nlohmann::json BuildPrePersistenceRejection(
    std::optional<std::string_view> request_id, ProtocolError error,
    TimePoint occurred_at, CommandType command_type = CommandType::kConfig);

std::string FormatUuidV4(std::array<std::uint8_t, 16> random_bytes);
std::string GenerateUuidV4();

}  // namespace cns::command
