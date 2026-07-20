#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/device/device_registry.hpp"

namespace cns::persistence {

enum class Urgency { kImmediate, kTelemetryBatch };

struct DesiredDeviceWrite {
  device::DeviceRecord record;
  std::uint64_t revision;
  bool write_metadata;
  bool write_status;
  bool write_telemetry;
  Urgency urgency;
};

class DirtyState {
 public:
  void Mark(DesiredDeviceWrite write);
  std::vector<DesiredDeviceWrite> TakeImmediate();
  std::vector<DesiredDeviceWrite> TakeTelemetryDue(
      std::chrono::steady_clock::time_point now,
      std::chrono::seconds interval);
  void Complete(std::string_view vendor_id, std::uint64_t revision);
  void Restore(DesiredDeviceWrite write);
  std::size_t Size() const;

 private:
  struct FieldState {
    std::uint64_t dirty_revision{};
    std::uint64_t in_flight_revision{};
  };

  struct Entry {
    device::DeviceRecord record;
    std::uint64_t revision{};
    FieldState metadata;
    FieldState status;
    FieldState telemetry;
    Urgency urgency{Urgency::kTelemetryBatch};
    std::chrono::steady_clock::time_point marked_at;
  };

  std::vector<DesiredDeviceWrite> TakeMatching(
      bool immediate,
      std::chrono::steady_clock::time_point now,
      std::chrono::seconds interval);
  static bool HasDirty(const Entry& entry);
  static bool HasWork(const Entry& entry);

  std::unordered_map<std::string, Entry> entries_;
};

}  // namespace cns::persistence
