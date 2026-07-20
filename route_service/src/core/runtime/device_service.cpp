#include "core/runtime/device_service.hpp"

#include <algorithm>
#include <utility>

#include "core/mqtt_topic/device_topic.hpp"
#include "core/protocol/device_message.hpp"

namespace cns::runtime {
namespace {
constexpr auto kOfflineScan = std::chrono::seconds{1};

persistence::DesiredDeviceWrite WriteFor(device::Mutation mutation,
                                         persistence::Urgency urgency) {
  const bool telemetry = mutation.reason == state_event::ChangeReason::kTelemetry;
  const auto revision = mutation.record.revision;
  return {.record = std::move(mutation.record),
          .revision = revision,
          .write_metadata = true,
          .write_status = !telemetry,
          .write_telemetry = telemetry,
          .urgency = urgency};
}
}  // namespace

DeviceService::DeviceService(device::DeviceRegistry& registry,
    ProvisionSubmitter provision, WriteSubmitter write, EventSink events,
    SteadyNow steady_now, DiagnosticSink diagnostic, std::size_t queue_capacity,
    std::string topic_namespace, std::chrono::seconds telemetry_interval,
    std::chrono::seconds offline_timeout)
    : registry_(registry), provision_(std::move(provision)),
      write_(std::move(write)), events_(std::move(events)),
      steady_now_(std::move(steady_now)), diagnostic_(std::move(diagnostic)),
      mqtt_queue_(queue_capacity), topic_namespace_(std::move(topic_namespace)),
      telemetry_interval_(telemetry_interval), offline_timeout_(offline_timeout) {
  try {
    next_scan_ = steady_now_() + kOfflineScan;
    scan_initialized_ = true;
  } catch (const std::exception&) {
    Diagnose("设备时钟端口初始化异常");
  } catch (...) {
    Diagnose("设备时钟端口初始化发生未知异常");
  }
}

bool DeviceService::TryPush(mqtt::InboundMessage message) {
  const bool pushed = mqtt_queue_.TryPush(std::move(message));
  if (pushed) Notify();
  return pushed;
}

void DeviceService::PushDatabaseResult(DatabaseResult result) {
  {
    std::lock_guard lock(results_mutex_);
    results_.push_back(std::move(result));
  }
  Notify();
}

void DeviceService::Run(std::stop_token stop) {
  std::stop_callback callback(stop, [this] { Notify(); });
  try {
    while (!stop.stop_requested()) {
      ProcessReady();
      bool no_results;
      {
        std::lock_guard results_lock(results_mutex_);
        no_results = results_.empty();
      }
      if (closed_ && mqtt_queue_.Size() == 0) {
        input_drained_ = true;
        Notify();
      }
      if (input_drained_ && no_results && !HasOutstandingDatabaseWork()) break;
      std::unique_lock lock(wake_mutex_);
      wake_.wait_for(lock, stop, std::chrono::milliseconds{100}, [this] {
        std::lock_guard results_lock(results_mutex_);
        return (!input_drained_ && closed_) || mqtt_queue_.Size() != 0 ||
               !results_.empty() || cancel_database_work_requested_;
      });
    }
    ProcessReady();
  } catch (const std::exception&) {
    Diagnose("设备业务线程端口异常，已安全停止");
  } catch (...) {
    Diagnose("设备业务线程发生未知异常，已安全停止");
  }
}

void DeviceService::Close() {
  closed_ = true;
  mqtt_queue_.Close();
  Notify();
}

bool DeviceService::WaitForInputDrained(std::chrono::milliseconds timeout) {
  std::unique_lock lock(wake_mutex_);
  return wake_.wait_for(lock, timeout, [this] { return input_drained_.load(); });
}

bool DeviceService::WaitForDatabaseIdle(std::chrono::milliseconds timeout) {
  std::unique_lock lock(wake_mutex_);
  return wake_.wait_for(lock, timeout, [this] {
    return outstanding_database_work_.load() == 0 && !processing_ready_.load();
  });
}

void DeviceService::CancelOutstandingDatabaseWork() {
  cancel_database_work_requested_ = true;
  Notify();
}

bool DeviceService::HasOutstandingDatabaseWork() const {
  return outstanding_database_work_.load() != 0;
}

void DeviceService::ProcessReady(TimePoint system_now) {
  struct ProcessingGuard {
    std::atomic_bool& processing;
    ~ProcessingGuard() { processing = false; }
  } guard{processing_ready_};
  processing_ready_ = true;
  for (;;) {
    std::optional<DatabaseResult> result;
    {
      std::lock_guard lock(results_mutex_);
      if (results_.empty()) break;
      result = std::move(results_.front());
      results_.pop_front();
    }
    Handle(std::move(*result));
  }
  if (cancel_database_work_requested_.exchange(false)) {
    for (auto& [vendor, candidate] : pending_) {
      static_cast<void>(vendor);
      candidate.submitted = false;
    }
    submitted_.clear();
    outstanding_database_work_ = 0;
    Notify();
  }
  while (mqtt_queue_.Size() != 0) {
    if (auto message = mqtt_queue_.WaitPop()) Handle(std::move(*message));
  }
  if (!database_unavailable_) {
    for (auto& [vendor, candidate] : pending_) {
      static_cast<void>(vendor);
      if (!candidate.submitted) {
        try {
          candidate.submitted = provision_(candidate.registration,
                                           candidate.received_at);
          if (candidate.submitted) ++outstanding_database_work_;
          if (!candidate.submitted) Diagnose("数据库建档提交端口已关闭");
        } catch (const std::exception&) {
          candidate.submitted = false;
          Diagnose("数据库建档提交回调异常");
        } catch (...) {
          candidate.submitted = false;
          Diagnose("数据库建档提交回调发生未知异常");
        }
      }
    }
  }
  const auto now = steady_now_();
  if (!scan_initialized_) {
    next_scan_ = now + kOfflineScan;
    scan_initialized_ = true;
  }
  if (now >= next_scan_) {
    for (auto& mutation : registry_.ExpireInactive(system_now, offline_timeout_)) {
      Mark(std::move(mutation));
    }
    next_scan_ = now + kOfflineScan;
  }
  DispatchWrites();
  Notify();
}

std::size_t DeviceService::PendingRegistrationCount() const { return pending_.size(); }
bool DeviceService::IsDatabaseUnavailable() const noexcept { return database_unavailable_; }
bool DeviceService::IsDeviceDegraded(std::string_view vendor_id) const {
  return database_unavailable_ || degraded_.contains(std::string{vendor_id});
}

void DeviceService::Handle(DatabaseResult result) {
  if (result.kind == DatabaseResult::Kind::kUnavailable) {
    database_unavailable_ = true;
    if (const auto* record = registry_.Find(result.vendor_id)) {
      degraded_[result.vendor_id] = std::max(record->revision, result.revision);
      const auto reason = last_reason_.contains(result.vendor_id)
          ? last_reason_.at(result.vendor_id)
          : state_event::ChangeReason::kTelemetry;
      Publish({*record, reason, true});
    }
    for (const auto& [vendor, candidate] : pending_) {
      static_cast<void>(vendor);
      if (candidate.submitted) --outstanding_database_work_;
    }
    pending_.clear();
    Notify();
    Diagnose("数据库暂不可用，设备状态进入降级模式");
    return;
  }
  if (result.kind == DatabaseResult::Kind::kPermanentFailure) {
    if (const auto pending = pending_.find(result.vendor_id);
        pending != pending_.end()) {
      if (pending->second.submitted) --outstanding_database_work_;
      pending_.erase(pending);
    }
    if (result.vendor_id.empty()) {
      for (const auto& [vendor, candidate] : pending_) {
        static_cast<void>(vendor);
        if (candidate.submitted) --outstanding_database_work_;
      }
      pending_.clear();
      outstanding_database_work_ -= submitted_.size();
      submitted_.clear();
    } else {
      if (submitted_.erase(result.vendor_id) != 0) {
        --outstanding_database_work_;
      }
    }
    if (const auto* record = registry_.Find(result.vendor_id)) {
      degraded_[result.vendor_id] = std::max(record->revision, result.revision);
      const auto reason = last_reason_.contains(result.vendor_id)
          ? last_reason_.at(result.vendor_id)
          : state_event::ChangeReason::kTelemetry;
      Publish({*record, reason, true});
    }
    Diagnose("数据库操作永久失败，已拒绝相关待处理操作");
    Notify();
    return;
  }
  if (result.kind == DatabaseResult::Kind::kRecovered) {
    database_unavailable_ = false;
    for (const auto& [vendor, revision] : degraded_) {
      const auto* record = registry_.Find(vendor);
      if (!record) continue;
      persistence::DesiredDeviceWrite write{*record, record->revision, true, true,
                                             record->latest_telemetry.has_value(),
                                             persistence::Urgency::kImmediate};
      SubmitWrite(std::move(write));
      static_cast<void>(revision);
    }
    return;
  }
  if (result.kind == DatabaseResult::Kind::kProvisioned && result.provisioned) {
    auto candidate = pending_.find(result.vendor_id);
    if (candidate == pending_.end()) return;
    const bool provision_was_outstanding = candidate->second.submitted;
    if (!registry_.AddProvisioned(std::move(*result.provisioned))) {
      if (provision_was_outstanding) --outstanding_database_work_;
      Notify();
      return;
    }
    auto registration = candidate->second;
    pending_.erase(candidate);
    if (auto mutation = registry_.ApplyRegistration(registration.registration,
                                                     registration.received_at)) {
      Mark(std::move(*mutation));
    }
    if (provision_was_outstanding) --outstanding_database_work_;
    Notify();
    return;
  }
  if (result.kind == DatabaseResult::Kind::kWriteCompleted) {
    const auto it = submitted_.find(result.vendor_id);
    if (it != submitted_.end() && result.revision >= it->second.revision) {
      dirty_.Complete(it->second);
      submitted_.erase(it);
      --outstanding_database_work_;
      Notify();
    }
    const auto degraded = degraded_.find(result.vendor_id);
    if (degraded != degraded_.end() && result.revision >= degraded->second) {
      degraded_.erase(degraded);
      if (const auto* record = registry_.Find(result.vendor_id)) {
        Publish({*record, state_event::ChangeReason::kDatabaseRecovered, false});
      }
    }
  }
}

void DeviceService::Handle(mqtt::InboundMessage message) {
  const auto topic = mqtt_topic::ParseDeviceTopic(topic_namespace_, message.topic);
  if (!topic) return;
  if (topic->kind == mqtt_topic::DeviceMessageKind::kRegistration) {
    auto registration = protocol::ParseRegistration(message.payload, topic->vendor_id);
    if (!registration) return;
    if (!registry_.Find(topic->vendor_id)) {
      if (!database_unavailable_) {
        const auto existing = pending_.find(topic->vendor_id);
        if (existing == pending_.end()) {
          pending_.emplace(topic->vendor_id,
                           PendingRegistration{std::move(*registration),
                                               message.received_at, false});
        } else {
          existing->second.registration = std::move(*registration);
          existing->second.received_at = message.received_at;
        }
      }
      return;
    }
    if (auto mutation = registry_.ApplyRegistration(*registration, message.received_at)) {
      Mark(std::move(*mutation));
    }
    return;
  }
  auto telemetry = protocol::ParseTelemetry(message.payload, topic->vendor_id);
  if (!telemetry) return;
  if (auto mutation = registry_.ApplyTelemetry(topic->vendor_id, std::move(*telemetry),
                                                message.received_at)) {
    Mark(std::move(*mutation));
  }
}

void DeviceService::Mark(device::Mutation mutation) {
  const auto reason = mutation.reason;
  const auto record = mutation.record;
  last_reason_[record.vendor_id] = reason;
  const bool telemetry = reason == state_event::ChangeReason::kTelemetry;
  dirty_.Mark(WriteFor(std::move(mutation), telemetry
      ? persistence::Urgency::kTelemetryBatch : persistence::Urgency::kImmediate),
      steady_now_());
  if (database_unavailable_ || degraded_.contains(record.vendor_id)) {
    degraded_[record.vendor_id] = record.revision;
  }
  Publish({record, reason, IsDeviceDegraded(record.vendor_id)});
}

void DeviceService::DispatchWrites() {
  if (database_unavailable_) return;
  auto send = [this](std::vector<persistence::DesiredDeviceWrite> writes) {
    for (auto& item : writes) {
      SubmitWrite(std::move(item));
    }
  };
  send(dirty_.TakeImmediate());
  send(dirty_.TakeTelemetryDue(steady_now_(), telemetry_interval_));
}

bool DeviceService::SubmitWrite(persistence::DesiredDeviceWrite write) {
  const auto vendor = write.record.vendor_id;
  try {
    if (!write_(write)) {
      dirty_.Restore(std::move(write));
      if (const auto* record = registry_.Find(vendor)) {
        degraded_[vendor] = record->revision;
        const auto reason = last_reason_.contains(vendor)
            ? last_reason_.at(vendor) : state_event::ChangeReason::kTelemetry;
        Publish({*record, reason, true});
      }
      Diagnose("数据库写入提交端口已关闭");
      return false;
    }
  } catch (const std::exception&) {
    dirty_.Restore(std::move(write));
    if (const auto* record = registry_.Find(vendor)) {
      degraded_[vendor] = record->revision;
      const auto reason = last_reason_.contains(vendor)
          ? last_reason_.at(vendor) : state_event::ChangeReason::kTelemetry;
      Publish({*record, reason, true});
    }
    Diagnose("数据库写入提交回调异常");
    return false;
  } catch (...) {
    dirty_.Restore(std::move(write));
    if (const auto* record = registry_.Find(vendor)) {
      degraded_[vendor] = record->revision;
      const auto reason = last_reason_.contains(vendor)
          ? last_reason_.at(vendor) : state_event::ChangeReason::kTelemetry;
      Publish({*record, reason, true});
    }
    Diagnose("数据库写入提交回调发生未知异常");
    return false;
  }
  if (!submitted_.contains(vendor)) ++outstanding_database_work_;
  submitted_[vendor] = std::move(write);
  return true;
}

void DeviceService::Publish(PublishedState state) noexcept {
  try { events_(std::move(state)); }
  catch (const std::exception&) { Diagnose("状态事件发布回调异常"); }
  catch (...) { Diagnose("状态事件发布回调发生未知异常"); }
}

void DeviceService::Diagnose(std::string message) noexcept {
  if (!diagnostic_) return;
  try { diagnostic_(std::move(message)); } catch (...) {}
}

void DeviceService::Notify() { wake_.notify_all(); }
}  // namespace cns::runtime
