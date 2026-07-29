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
    case ChangeReason::kSnapshotReplay: return "snapshot_replay";
  }
  return "";
}

nlohmann::json OptionalTime(
    const std::optional<std::chrono::system_clock::time_point>& value) {
  return value ? nlohmann::json(FormatUtcRfc3339Millis(*value))
               : nlohmann::json(nullptr);
}
}  // namespace

nlohmann::json BuildDeviceDirectorySnapshot(
    const std::vector<DirectoryEntry>& entries, std::uint64_t revision,
    std::chrono::system_clock::time_point generated_at) {
  auto sorted_entries = entries;
  std::ranges::sort(sorted_entries, {}, &DirectoryEntry::vendor_id);
  auto devices = nlohmann::json::array();
  for (const auto& entry : sorted_entries) {
    devices.push_back({
        {"device_id", entry.vendor_id},
        {"device_type", entry.device_type},
        {"vendor_id", entry.vendor_id},
        {"school_name", entry.school_name.empty()
                            ? nlohmann::json(nullptr)
                            : nlohmann::json(entry.school_name)},
        {"dcdw_label", entry.dcdw_label
                           ? nlohmann::json(*entry.dcdw_label)
                           : nlohmann::json(nullptr)},
        {"model_version", entry.model_version},
        {"status", entry.online ? "online" : "offline"},
    });
  }
  return {{"schema_version", 1},
          {"event_type", "device_directory_snapshot"},
          {"generated_at", FormatUtcRfc3339Millis(generated_at)},
          {"revision", revision},
          {"devices", std::move(devices)}};
}

nlohmann::json BuildStateEvent(
    const Snapshot& snapshot, ChangeReason reason,
    std::chrono::system_clock::time_point event_at) {
  return {{"schema_version", 1}, {"event_type", "device_state"},
          {"event_at", FormatUtcRfc3339Millis(event_at)},
          {"revision", snapshot.revision},
          {"device_id", snapshot.vendor_id},
          {"device_type", snapshot.device_type},
          {"vendor_id", snapshot.vendor_id},
          {"school_name", snapshot.school_name.empty()
                              ? nlohmann::json(nullptr)
                              : nlohmann::json(snapshot.school_name)},
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

bool DeviceDirectory::Update(DirectoryEntry entry) {
  const auto iterator = entries_.find(entry.vendor_id);
  const bool changed =
      iterator == entries_.end() || iterator->second != entry;
  if (!initialized_ || changed) {
    entries_.insert_or_assign(entry.vendor_id, std::move(entry));
    initialized_ = true;
    ++revision_;
    return true;
  }
  return false;
}

bool DeviceDirectory::Replace(
    const std::vector<DirectoryEntry>& entries) {
  std::map<std::string, DirectoryEntry> replacement;
  for (const auto& entry : entries) {
    replacement.insert_or_assign(entry.vendor_id, entry);
  }
  if (initialized_ && replacement == entries_) return false;
  entries_ = std::move(replacement);
  initialized_ = true;
  ++revision_;
  return true;
}

std::vector<DirectoryEntry> DeviceDirectory::Entries() const {
  std::vector<DirectoryEntry> entries;
  entries.reserve(entries_.size());
  for (const auto& [vendor_id, entry] : entries_) {
    static_cast<void>(vendor_id);
    entries.push_back(entry);
  }
  return entries;
}

std::uint64_t DeviceDirectory::Revision() const noexcept {
  return revision_;
}

}  // namespace cns::state_event
