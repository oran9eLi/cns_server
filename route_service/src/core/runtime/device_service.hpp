#pragma once

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <deque>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <unordered_map>

#include "adapters/mqtt/mqtt_client.hpp"
#include "core/device/device_registry.hpp"
#include "core/persistence/dirty_state.hpp"
#include "core/queue/bounded_queue.hpp"

namespace cns::runtime {

using TimePoint = std::chrono::system_clock::time_point;

struct DatabaseResult {
  enum class Kind {
    kProvisioned, kWriteCompleted, kUnavailable, kRecovered, kPermanentFailure
  };
  Kind kind;
  std::string vendor_id;
  std::uint64_t revision;
  std::optional<device::DeviceRecord> provisioned;
  std::string error;
};

struct PublishedState {
  device::DeviceRecord record;
  state_event::ChangeReason reason;
  bool degraded;
};

class DeviceService {
 public:
  using ProvisionSubmitter = std::function<bool(protocol::Registration, TimePoint)>;
  using WriteSubmitter = std::function<bool(persistence::DesiredDeviceWrite)>;
  using EventSink = std::function<void(PublishedState)>;
  using SteadyNow = std::function<std::chrono::steady_clock::time_point()>;
  using DiagnosticSink = std::function<void(std::string)>;

  DeviceService(device::DeviceRegistry& registry, ProvisionSubmitter provision,
                WriteSubmitter write, EventSink events, SteadyNow steady_now,
                DiagnosticSink diagnostic = {},
                std::size_t queue_capacity = 1024,
                std::string topic_namespace = "cns",
                std::chrono::seconds telemetry_interval = std::chrono::seconds{5},
                std::chrono::seconds offline_timeout = std::chrono::seconds{60});

  bool TryPush(mqtt::InboundMessage message);
  void PushDatabaseResult(DatabaseResult result);
  void Run(std::stop_token stop);
  void Close();

  // 可确定驱动的测试入口；生产运行仍由 Run 独占调用。
  void ProcessReady(TimePoint system_now = std::chrono::system_clock::now());
  [[nodiscard]] std::size_t PendingRegistrationCount() const;
  [[nodiscard]] bool IsDatabaseUnavailable() const noexcept;
  [[nodiscard]] bool IsDeviceDegraded(std::string_view vendor_id) const;

 private:
  struct PendingRegistration {
    protocol::Registration registration;
    TimePoint received_at;
    bool submitted{};
  };

  void Handle(DatabaseResult result);
  void Handle(mqtt::InboundMessage message);
  void Mark(device::Mutation mutation);
  void DispatchWrites();
  bool SubmitWrite(persistence::DesiredDeviceWrite write);
  void Publish(PublishedState state) noexcept;
  void Diagnose(std::string message) noexcept;
  void Notify();

  device::DeviceRegistry& registry_;
  ProvisionSubmitter provision_;
  WriteSubmitter write_;
  EventSink events_;
  SteadyNow steady_now_;
  DiagnosticSink diagnostic_;
  queue::BoundedQueue<mqtt::InboundMessage> mqtt_queue_;
  std::deque<DatabaseResult> results_;
  mutable std::mutex results_mutex_;
  persistence::DirtyState dirty_;
  std::unordered_map<std::string, PendingRegistration> pending_;
  std::unordered_map<std::string, std::uint64_t> degraded_;
  std::unordered_map<std::string, state_event::ChangeReason> last_reason_;
  std::unordered_map<std::string, persistence::DesiredDeviceWrite> submitted_;
  std::chrono::steady_clock::time_point next_scan_{};
  bool scan_initialized_ = false;
  bool database_unavailable_ = false;
  std::atomic_bool closed_{false};
  std::string topic_namespace_;
  std::chrono::seconds telemetry_interval_;
  std::chrono::seconds offline_timeout_;
  mutable std::mutex wake_mutex_;
  std::condition_variable_any wake_;
};

}  // namespace cns::runtime
