#include "core/runtime/device_service.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "core/mqtt_topic/device_topic.hpp"
#include "core/protocol/device_message.hpp"

namespace cns::runtime {
namespace {
constexpr auto kOfflineScan = std::chrono::seconds{1};

std::string DescribeDeviceForLog(const device::DeviceRecord& record) {
  std::string text = record.school_name + "/";
  text += record.dcdw_label ? *record.dcdw_label : "未编号";
  text += " vendor_id=";
  text += record.vendor_id;
  return text;
}

persistence::DesiredDeviceWrite WriteFor(device::Mutation mutation,
                                         persistence::Urgency urgency) {
  const bool telemetry = mutation.reason == state_event::ChangeReason::kTelemetry;
  const auto revision = mutation.record.revision;
  return {.record = std::move(mutation.record),
          .revision = revision,
          .write_metadata = true,
          .write_status = !telemetry || mutation.status_changed,
          .write_telemetry = telemetry,
          .urgency = urgency};
}
}  // namespace

DeviceService::DeviceService(device::DeviceRegistry& registry,
    ProvisionSubmitter provision, WriteSubmitter write, EventSink events,
    SteadyNow steady_now, DiagnosticSink diagnostic, std::size_t queue_capacity,
    std::string topic_namespace, std::chrono::seconds telemetry_interval,
    std::chrono::seconds offline_timeout, InformationSink information,
    SnapshotSink snapshots)
    : registry_(registry), provision_(std::move(provision)),
      write_(std::move(write)), events_(std::move(events)),
      snapshots_(std::move(snapshots)),
      steady_now_(std::move(steady_now)), diagnostic_(std::move(diagnostic)),
      mqtt_queue_(queue_capacity), topic_namespace_(std::move(topic_namespace)),
      telemetry_interval_(telemetry_interval), offline_timeout_(offline_timeout),
      information_(std::move(information)) {
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

void DeviceService::Run(std::stop_token stop, CycleHook cycle_hook) {
  std::stop_callback callback(stop, [this] { Notify(); });
  try {
    while (!stop.stop_requested()) {
      ProcessReady();
      if (cycle_hook) cycle_hook(std::chrono::system_clock::now());
      bool no_results;
      {
        std::lock_guard results_lock(results_mutex_);
        no_results = results_.empty();
      }
      if (IsClosed() && mqtt_queue_.Size() == 0) {
        SetInputDrained();
        ProcessReady();
        if (cycle_hook) cycle_hook(std::chrono::system_clock::now());
      }
      if (IsInputDrained() && no_results && !HasOutstandingDatabaseWork()) break;
      std::unique_lock lock(wake_mutex_);
      wake_.wait_for(lock, stop, std::chrono::milliseconds{100}, [this] {
        std::lock_guard results_lock(results_mutex_);
        return (!input_drained_ && closed_) || mqtt_queue_.Size() != 0 ||
               !results_.empty() || cancel_database_work_requested_ ||
               external_snapshot_requested_;
      });
    }
    ProcessReady();
    if (cycle_hook) cycle_hook(std::chrono::system_clock::now());
  } catch (const std::exception&) {
    Diagnose("设备业务线程端口异常，已安全停止");
  } catch (...) {
    Diagnose("设备业务线程发生未知异常，已安全停止");
  }
}

void DeviceService::Close() {
  {
    std::lock_guard lock(wake_mutex_);
    closed_ = true;
    external_snapshot_requested_ = false;
    drain_dispatch_pending_ = true;
  }
  mqtt_queue_.Close();
  Notify();
}

bool DeviceService::WaitForInputDrained(std::chrono::milliseconds timeout) {
  std::unique_lock lock(wake_mutex_);
  return wake_.wait_for(lock, timeout, [this] { return input_drained_; });
}

bool DeviceService::WaitForDatabaseIdle(std::chrono::milliseconds timeout) {
  std::unique_lock lock(wake_mutex_);
  if (outstanding_database_work_ == 0 && !processing_ready_ &&
      !drain_dispatch_pending_) return true;
  if (before_database_idle_wait_for_testing_) {
    before_database_idle_wait_for_testing_();
  }
  return wake_.wait_for(lock, timeout, [this] {
    return outstanding_database_work_ == 0 && !processing_ready_ &&
           !drain_dispatch_pending_;
  });
}

void DeviceService::SetBeforeDatabaseIdleWaitHookForTesting(
    std::function<void()> hook) {
  std::lock_guard lock(wake_mutex_);
  before_database_idle_wait_for_testing_ = std::move(hook);
}

void DeviceService::CancelOutstandingDatabaseWork() {
  {
    std::lock_guard lock(wake_mutex_);
    cancel_database_work_requested_ = true;
    drain_dispatch_pending_ = false;
  }
  Notify();
}

void DeviceService::RequestExternalSnapshot() {
  {
    std::lock_guard lock(wake_mutex_);
    if (closed_) return;
    external_snapshot_requested_ = true;
  }
  Notify();
}

bool DeviceService::IsClosed() const {
  std::lock_guard lock(wake_mutex_);
  return closed_;
}

bool DeviceService::ConsumeCancelDatabaseWorkRequest() {
  std::lock_guard lock(wake_mutex_);
  return std::exchange(cancel_database_work_requested_, false);
}

bool DeviceService::ConsumeExternalSnapshotRequest() {
  std::lock_guard lock(wake_mutex_);
  return std::exchange(external_snapshot_requested_, false);
}

void DeviceService::SetDrainDispatchPending(bool value) {
  {
    std::lock_guard lock(wake_mutex_);
    drain_dispatch_pending_ = value;
  }
  Notify();
}

bool DeviceService::HasOutstandingDatabaseWork() const {
  std::lock_guard lock(wake_mutex_);
  return outstanding_database_work_ != 0 || drain_dispatch_pending_;
}

bool DeviceService::IsInputDrained() const {
  std::lock_guard lock(wake_mutex_);
  return input_drained_;
}

void DeviceService::SetInputDrained() {
  {
    std::lock_guard lock(wake_mutex_);
    input_drained_ = true;
  }
  Notify();
}

void DeviceService::SetProcessingReady(bool value) {
  {
    std::lock_guard lock(wake_mutex_);
    processing_ready_ = value;
  }
  Notify();
}

void DeviceService::AdjustOutstandingDatabaseWork(std::ptrdiff_t delta) {
  {
    std::lock_guard lock(wake_mutex_);
    if (delta < 0) {
      outstanding_database_work_ -= static_cast<std::size_t>(-delta);
    } else {
      outstanding_database_work_ += static_cast<std::size_t>(delta);
    }
  }
  Notify();
}

void DeviceService::ResetOutstandingDatabaseWork() {
  {
    std::lock_guard lock(wake_mutex_);
    outstanding_database_work_ = 0;
  }
  Notify();
}

void DeviceService::ProcessReady(TimePoint system_now) {
  const auto finish_processing = [this] {
    SetProcessingReady(false);
  };
  struct ProcessingGuard {
    const decltype(finish_processing)& finish;
    ~ProcessingGuard() { finish(); }
  } guard{finish_processing};
  SetProcessingReady(true);
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
  if (ConsumeCancelDatabaseWorkRequest()) {
    for (auto& [vendor, candidate] : pending_) {
      static_cast<void>(vendor);
      candidate.submitted = false;
    }
    submitted_.clear();
    dirty_.Clear();
    drain_submission_blocked_ = false;
    ResetOutstandingDatabaseWork();
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
          if (candidate.submitted) AdjustOutstandingDatabaseWork(1);
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
  if (ConsumeExternalSnapshotRequest()) PublishCurrentSnapshot();
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
    std::ptrdiff_t completed = 0;
    for (const auto& [vendor, candidate] : pending_) {
      static_cast<void>(vendor);
      if (candidate.submitted) ++completed;
    }
    pending_.clear();
    if (completed != 0) AdjustOutstandingDatabaseWork(-completed);
    Diagnose("数据库暂不可用，设备状态进入降级模式");
    return;
  }
  if (result.kind == DatabaseResult::Kind::kPermanentFailure) {
    if (const auto pending = pending_.find(result.vendor_id);
        pending != pending_.end()) {
      if (pending->second.submitted) AdjustOutstandingDatabaseWork(-1);
      pending_.erase(pending);
    }
    if (result.vendor_id.empty()) {
      std::ptrdiff_t completed = 0;
      for (const auto& [vendor, candidate] : pending_) {
        static_cast<void>(vendor);
        if (candidate.submitted) ++completed;
      }
      pending_.clear();
      completed += static_cast<std::ptrdiff_t>(submitted_.size());
      submitted_.clear();
      pending_status_logs_.clear();
      if (completed != 0) AdjustOutstandingDatabaseWork(-completed);
    } else {
      if (submitted_.erase(result.vendor_id) != 0) {
        AdjustOutstandingDatabaseWork(-1);
      }
      pending_status_logs_.erase(result.vendor_id);
    }
    if (const auto* record = registry_.Find(result.vendor_id)) {
      degraded_[result.vendor_id] = std::max(record->revision, result.revision);
      const auto reason = last_reason_.contains(result.vendor_id)
          ? last_reason_.at(result.vendor_id)
          : state_event::ChangeReason::kTelemetry;
      Publish({*record, reason, true});
    }
    Diagnose("数据库操作永久失败，已拒绝相关待处理操作");
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
      if (provision_was_outstanding) AdjustOutstandingDatabaseWork(-1);
      return;
    }
    auto registration = candidate->second;
    pending_.erase(candidate);
    if (auto mutation = registry_.ApplyRegistration(registration.registration,
                                                     registration.received_at)) {
      Mark(std::move(*mutation));
    }
    if (provision_was_outstanding) AdjustOutstandingDatabaseWork(-1);
    return;
  }
  if (result.kind == DatabaseResult::Kind::kWriteCompleted) {
    const auto it = submitted_.find(result.vendor_id);
    if (it != submitted_.end() && result.revision >= it->second.revision) {
      const auto status_log = pending_status_logs_.find(result.vendor_id);
      if (status_log != pending_status_logs_.end() &&
          result.revision >= status_log->second.revision &&
          it->second.write_status) {
        const auto& record = status_log->second.record;
        Inform(std::string{record.status == device::Status::kOnline
                               ? "设备上线："
                               : "设备离线："} +
               DescribeDeviceForLog(record));
        pending_status_logs_.erase(status_log);
      }
      dirty_.Complete(it->second);
      submitted_.erase(it);
      AdjustOutstandingDatabaseWork(-1);
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
  if (mutation.status_changed) {
    pending_status_logs_[record.vendor_id] =
        persistence::DesiredDeviceWrite{record, record.revision, false, true,
                                        false, persistence::Urgency::kImmediate};
  }
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
    bool accepted = true;
    for (auto& item : writes) {
      accepted = SubmitWrite(std::move(item)) && accepted;
    }
    return accepted;
  };
  if (IsInputDrained()) {
    if (drain_submission_blocked_) return;
    if (send(dirty_.TakeAllDirty())) {
      SetDrainDispatchPending(false);
    } else {
      drain_submission_blocked_ = true;
    }
  } else {
    static_cast<void>(send(dirty_.TakeImmediate()));
    static_cast<void>(
        send(dirty_.TakeTelemetryDue(steady_now_(), telemetry_interval_)));
  }
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
  if (!submitted_.contains(vendor)) AdjustOutstandingDatabaseWork(1);
  submitted_[vendor] = std::move(write);
  return true;
}

void DeviceService::Publish(PublishedState state) noexcept {
  try { events_(std::move(state)); }
  catch (const std::exception&) { Diagnose("状态事件发布回调异常"); }
  catch (...) { Diagnose("状态事件发布回调发生未知异常"); }
}

void DeviceService::PublishCurrentSnapshot() noexcept {
  if (!snapshots_) return;
  try {
    std::vector<PublishedState> states;
    for (auto& record : registry_.ListOnlineDevices()) {
      const auto reason = last_reason_.contains(record.vendor_id)
          ? last_reason_.at(record.vendor_id)
          : record.latest_telemetry
                ? state_event::ChangeReason::kTelemetry
                : state_event::ChangeReason::kRegistrationOnline;
      states.push_back(
          {std::move(record), reason, IsDeviceDegraded(record.vendor_id)});
    }
    snapshots_(std::move(states));
  } catch (const std::exception&) {
    Diagnose("设备全量快照发布回调异常");
  } catch (...) {
    Diagnose("设备全量快照发布回调发生未知异常");
  }
}

void DeviceService::Diagnose(std::string message) noexcept {
  if (!diagnostic_) return;
  try { diagnostic_(std::move(message)); } catch (...) {}
}

void DeviceService::Inform(std::string message) noexcept {
  if (!information_) return;
  try { information_(std::move(message)); } catch (...) {}
}

void DeviceService::Notify() { wake_.notify_all(); }
}  // namespace cns::runtime
