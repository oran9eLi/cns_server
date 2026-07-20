#pragma once

#include <chrono>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace cns::state_event {

enum class ChangeReason {
  kRegistrationOnline,
  kRegistrationOffline,
  kTelemetry,
  kActivityTimeout,
  kDatabaseRecovered,
};

struct Snapshot {
  std::string vendor_id;
  std::string school_name;
  std::optional<std::string> dcdw_label;
  bool online;
  std::optional<std::chrono::system_clock::time_point> last_seen_at;
  std::optional<std::chrono::system_clock::time_point> telemetry_received_at;
  std::optional<nlohmann::json> latest_telemetry;
  bool degraded;
};

std::string FormatUtcRfc3339Millis(
    std::chrono::system_clock::time_point value);
nlohmann::json BuildStateEvent(
    const Snapshot& snapshot, ChangeReason reason,
    std::chrono::system_clock::time_point event_at);

}  // namespace cns::state_event
