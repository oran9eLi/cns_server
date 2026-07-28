#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

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
  std::uint64_t revision;
  std::int64_t school_id;
  std::string school_name;
  std::optional<std::string> dcdw_label;
  std::string model_version;
  bool online;
  std::optional<std::chrono::system_clock::time_point> last_seen_at;
  std::optional<std::chrono::system_clock::time_point> telemetry_received_at;
  std::optional<nlohmann::json> latest_telemetry;
  bool degraded;
};

class OnlineDeviceDirectory {
 public:
  bool Update(std::string_view vendor_id, bool online);
  bool Replace(const std::vector<std::string>& vendor_ids);
  [[nodiscard]] std::vector<std::string> DeviceIds() const;
  [[nodiscard]] std::uint64_t Revision() const noexcept;

 private:
  std::set<std::string> vendor_ids_;
  std::uint64_t revision_ = 0;
  bool initialized_ = false;
};

std::string FormatUtcRfc3339Millis(
    std::chrono::system_clock::time_point value);
nlohmann::json BuildStateEvent(
    const Snapshot& snapshot, ChangeReason reason,
    std::chrono::system_clock::time_point event_at);
nlohmann::json BuildOnlineDeviceSnapshot(
    const std::vector<std::string>& vendor_ids, std::uint64_t revision,
    std::chrono::system_clock::time_point generated_at);

}  // namespace cns::state_event
