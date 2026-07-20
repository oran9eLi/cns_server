#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/protocol/device_message.hpp"
#include "core/state_event/state_event.hpp"

namespace cns::device {

using TimePoint = std::chrono::system_clock::time_point;

enum class Status { kOnline, kOffline };

struct DeviceRecord {
  std::string vendor_id;
  std::int64_t school_id;
  std::string school_name;
  std::optional<std::string> dcdw_label;
  std::string model_version;
  Status status;
  std::optional<TimePoint> last_seen_at;
  std::optional<nlohmann::json> latest_telemetry;
  std::optional<TimePoint> telemetry_received_at;
  std::uint64_t revision;
};

struct Mutation {
  DeviceRecord record;
  state_event::ChangeReason reason;
  bool status_changed;
  std::optional<std::string> diagnostic;
};

class DeviceRegistry {
 public:
  std::expected<void, std::string> Load(std::vector<DeviceRecord> records);
  std::expected<Mutation, std::string> ApplyRegistration(
      const protocol::Registration& registration, TimePoint received_at);
  std::expected<Mutation, std::string> ApplyTelemetry(
      std::string_view vendor_id, protocol::Telemetry telemetry,
      TimePoint received_at);
  std::vector<Mutation> ExpireInactive(TimePoint now,
                                       std::chrono::seconds timeout);
  std::expected<void, std::string> AddProvisioned(DeviceRecord record);
  // 返回的指针只保证在本目录下一次修改操作前有效。
  const DeviceRecord* Find(std::string_view vendor_id) const;

 private:
  struct RoleKey {
    std::int64_t school_id;
    std::string dcdw_label;

    bool operator==(const RoleKey&) const = default;
  };

  struct RoleKeyHash {
    std::size_t operator()(const RoleKey& key) const noexcept;
  };

  std::unordered_map<std::string, DeviceRecord> records_;
  std::unordered_map<RoleKey, std::string, RoleKeyHash> roles_;
};

}  // namespace cns::device
