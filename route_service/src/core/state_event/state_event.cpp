#include "core/state_event/state_event.hpp"

#include <algorithm>

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

nlohmann::json BuildOnlineDeviceSnapshot(
    const std::vector<std::string>& vendor_ids, std::uint64_t revision,
    std::chrono::system_clock::time_point generated_at) {
  auto sorted_ids = vendor_ids;
  std::ranges::sort(sorted_ids);
  sorted_ids.erase(std::unique(sorted_ids.begin(), sorted_ids.end()),
                   sorted_ids.end());
  return {{"schema_version", 1},
          {"event_type", "online_device_snapshot"},
          {"generated_at", FormatUtcRfc3339Millis(generated_at)},
          {"revision", revision},
          {"device_ids", std::move(sorted_ids)}};
}

nlohmann::json BuildStateEvent(
    const Snapshot& snapshot, ChangeReason reason,
    std::chrono::system_clock::time_point event_at) {
  return {{"schema_version", 1}, {"event_type", "device_state"},
          {"event_at", FormatUtcRfc3339Millis(event_at)},
          {"revision", snapshot.revision},
          {"vendor_id", snapshot.vendor_id},
          {"school_id", snapshot.school_id},
          {"school_name", snapshot.school_name},
          {"dcdw_label", snapshot.dcdw_label ? nlohmann::json(*snapshot.dcdw_label)
                                               : nlohmann::json(nullptr)},
          {"model_version", snapshot.model_version},
          {"status", snapshot.online ? "online" : "offline"},
          {"last_seen_at", OptionalTime(snapshot.last_seen_at)},
          {"telemetry_received_at", OptionalTime(snapshot.telemetry_received_at)},
          {"latest_telemetry", snapshot.latest_telemetry ? *snapshot.latest_telemetry
                                                          : nlohmann::json(nullptr)},
          {"change_reason", ToString(reason)}, {"degraded", snapshot.degraded}};
}

bool OnlineDeviceDirectory::Update(std::string_view vendor_id, bool online) {
  bool changed = false;
  if (online) {
    changed = vendor_ids_.insert(std::string{vendor_id}).second;
  } else {
    changed = vendor_ids_.erase(std::string{vendor_id}) != 0;
  }
  if (!initialized_ || changed) {
    initialized_ = true;
    ++revision_;
    return true;
  }
  return false;
}

bool OnlineDeviceDirectory::Replace(
    const std::vector<std::string>& vendor_ids) {
  const std::set<std::string> replacement(vendor_ids.begin(), vendor_ids.end());
  if (initialized_ && replacement == vendor_ids_) return false;
  vendor_ids_ = replacement;
  initialized_ = true;
  ++revision_;
  return true;
}

std::vector<std::string> OnlineDeviceDirectory::DeviceIds() const {
  return {vendor_ids_.begin(), vendor_ids_.end()};
}

std::uint64_t OnlineDeviceDirectory::Revision() const noexcept {
  return revision_;
}

}  // namespace cns::state_event
