#include "core/state_event/state_event.hpp"

#include "core/protocol/time_format.hpp"

namespace cns::state_event {

std::string FormatUtcRfc3339Millis(
    std::chrono::system_clock::time_point value) {
  return protocol::FormatUtcRfc3339Millis(value);
}

namespace {
const char* ToString(ChangeReason reason) {
  switch (reason) {
    case ChangeReason::kRegistrationOnline: return "registration_online";
    case ChangeReason::kRegistrationOffline: return "registration_offline";
    case ChangeReason::kTelemetry: return "telemetry";
    case ChangeReason::kActivityTimeout: return "activity_timeout";
    case ChangeReason::kDatabaseRecovered: return "database_recovered";
  }
  return "";
}

nlohmann::json OptionalTime(
    const std::optional<std::chrono::system_clock::time_point>& value) {
  return value ? nlohmann::json(FormatUtcRfc3339Millis(*value))
               : nlohmann::json(nullptr);
}
}  // namespace

nlohmann::json BuildStateEvent(
    const Snapshot& snapshot, ChangeReason reason,
    std::chrono::system_clock::time_point event_at) {
  return {{"schema_version", 1}, {"event_type", "device_state"},
          {"event_at", FormatUtcRfc3339Millis(event_at)},
          {"vendor_id", snapshot.vendor_id}, {"school_name", snapshot.school_name},
          {"dcdw_label", snapshot.dcdw_label ? nlohmann::json(*snapshot.dcdw_label)
                                               : nlohmann::json(nullptr)},
          {"status", snapshot.online ? "online" : "offline"},
          {"last_seen_at", OptionalTime(snapshot.last_seen_at)},
          {"telemetry_received_at", OptionalTime(snapshot.telemetry_received_at)},
          {"latest_telemetry", snapshot.latest_telemetry ? *snapshot.latest_telemetry
                                                          : nlohmann::json(nullptr)},
          {"change_reason", ToString(reason)}, {"degraded", snapshot.degraded}};
}

}  // namespace cns::state_event
