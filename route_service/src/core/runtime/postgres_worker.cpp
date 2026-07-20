#include "core/runtime/postgres_worker.hpp"

#include <algorithm>
#include <exception>
#include <type_traits>
#include <utility>

namespace cns::runtime {
namespace {
constexpr auto kReconnectUnavailable = "数据库重连暂不可用";
constexpr auto kMigrationPermanent = "数据库迁移版本校验永久失败";
constexpr auto kProvisionUnavailable = "数据库建档暂不可用";
constexpr auto kProvisionPermanent = "数据库建档永久失败";
constexpr auto kWriteUnavailable = "数据库写入暂不可用";
constexpr auto kWritePermanent = "数据库写入永久失败";
}  // namespace

PostgresWorker::PostgresWorker(StorePort& store, ResultSink results,
    ReplayRequest replay, SteadyNow steady_now, DiagnosticSink diagnostic,
    std::chrono::seconds reconnect_interval,
    CommandResultSink command_results, std::size_t max_inflight_commands)
    : store_(store), results_(std::move(results)), replay_(std::move(replay)),
      steady_now_(std::move(steady_now)), diagnostic_(std::move(diagnostic)),
      command_results_(std::move(command_results)),
      max_inflight_commands_(max_inflight_commands),
      reconnect_interval_(reconnect_interval) {}

bool PostgresWorker::SubmitProvision(protocol::Registration registration,
                                     TimePoint at) {
  std::lock_guard lock(mutex_);
  if (!accepting_ || unavailable_) return false;
  const auto vendor = registration.vendor_id;
  if (provisioning_.contains(vendor)) return false;
  provisions_.insert_or_assign(vendor,
                               ProvisionTask{std::move(registration), at});
  changed_.notify_one();
  return true;
}

bool PostgresWorker::SubmitWrite(persistence::DesiredDeviceWrite write) {
  std::lock_guard lock(mutex_);
  if (!accepting_) return false;
  MergeWrite(std::move(write));
  changed_.notify_one();
  return true;
}

bool PostgresWorker::SubmitCommand(std::uint64_t operation_id,
                                   CommandDatabaseTask task) {
  std::lock_guard lock(mutex_);
  if (!accepting_ || command_operations_.contains(operation_id) ||
      command_operations_.size() >= max_inflight_commands_) {
    return false;
  }
  command_operations_.insert(operation_id);
  commands_.emplace_back(operation_id, std::move(task));
  changed_.notify_one();
  return true;
}

void PostgresWorker::Run(std::stop_token stop) {
  std::stop_callback callback(stop, [this] { changed_.notify_all(); });
  try {
    for (;;) {
      std::optional<ProvisionTask> provision;
      std::optional<persistence::DesiredDeviceWrite> write;
      std::optional<std::pair<std::uint64_t, CommandDatabaseTask>> command_task;
      bool reconnect = false;
      {
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, std::chrono::milliseconds{100}, [&] {
          return stop.stop_requested() ||
                 (drain_requested_ && !unavailable_ && !HasWork()) ||
                 (!unavailable_ && HasWork()) ||
                 (unavailable_ && steady_now_() >= reconnect_at_);
        });
        if (stop.stop_requested()) {
          Finish();
          return;
        }
        if (unavailable_) {
          if (steady_now_() < reconnect_at_) continue;
          reconnect = true;
        } else if (!commands_.empty()) {
          command_task = std::move(commands_.front());
          commands_.pop_front();
        } else if (!provisions_.empty()) {
          auto node = provisions_.extract(provisions_.begin());
          provisioning_.insert(node.key());
          provision = std::move(node.mapped());
        } else if (!writes_.empty()) {
          auto node = writes_.extract(writes_.begin());
          write = std::move(node.mapped());
        } else if (drain_requested_) {
          Finish();
          return;
        }
      }

      if (!reconnect && !command_task && !provision && !write) continue;

      if (reconnect) {
        std::expected<void, DatabaseError> result;
        try {
          result = store_.ReconnectAndValidate();
        } catch (const std::exception&) {
          result = std::unexpected(DatabaseError{
              DatabaseError::Kind::kPermanent, "数据库重连校验端口异常"});
        } catch (...) {
          result = std::unexpected(DatabaseError{
              DatabaseError::Kind::kPermanent, "数据库重连校验发生未知异常"});
        }
        if (!result) {
          if (result.error().kind == DatabaseError::Kind::kUnavailable) {
            {
              std::lock_guard lock(mutex_);
              reconnect_at_ = steady_now_() + reconnect_interval_;
            }
            Diagnose(kReconnectUnavailable);
            continue;
          }
          Emit({DatabaseResult::Kind::kPermanentFailure, {}, 0, std::nullopt,
                kMigrationPermanent});
          Diagnose(kMigrationPermanent);
          std::lock_guard lock(mutex_);
          accepting_ = false;
          provisions_.clear();
          writes_.clear();
          commands_.clear();
          command_operations_.clear();
          Finish();
          return;
        }
        {
          std::lock_guard lock(mutex_);
          unavailable_ = false;
        }
        Emit({DatabaseResult::Kind::kRecovered, {}, 0, std::nullopt, {}});
        try {
          replay_();
        } catch (const std::exception&) {
          Diagnose("registration retained replay 回调异常");
        } catch (...) {
          Diagnose("registration retained replay 回调发生未知异常");
        }
        continue;
      }

      if (command_task) {
        std::expected<CommandDatabaseValue, DatabaseError> result =
            std::unexpected(DatabaseError{DatabaseError::Kind::kPermanent,
                                          "数据库命令端口异常"});
        try {
          result = std::visit(
              [this](const auto& task)
                  -> std::expected<CommandDatabaseValue, DatabaseError> {
                using Task = std::decay_t<decltype(task)>;
                if constexpr (std::is_same_v<Task, FindCommandTask>) {
                  auto value = store_.FindCommand(task.source_id, task.request_id);
                  if (!value) return std::unexpected(value.error());
                  return CommandDatabaseValue{std::move(*value)};
                } else if constexpr (std::is_same_v<Task, InsertCommandTask>) {
                  auto value = store_.InsertCommand(task.command);
                  if (!value) return std::unexpected(value.error());
                  return CommandDatabaseValue{std::move(*value)};
                } else if constexpr (std::is_same_v<Task, TransitionCommandTask>) {
                  auto value = store_.TransitionCommand(
                      task.command_id, task.expected, task.desired, task.update);
                  if (!value) return std::unexpected(value.error());
                  return CommandDatabaseValue{std::move(*value)};
                } else {
                  auto value = store_.CleanupCommands(task.before, task.batch_size);
                  if (!value) return std::unexpected(value.error());
                  return CommandDatabaseValue{*value};
                }
              },
              command_task->second);
        } catch (const std::exception&) {
          result = std::unexpected(DatabaseError{
              DatabaseError::Kind::kPermanent, "数据库命令端口异常"});
        } catch (...) {
          result = std::unexpected(DatabaseError{
              DatabaseError::Kind::kPermanent, "数据库命令端口发生未知异常"});
        }
        if (!result && result.error().kind == DatabaseError::Kind::kUnavailable) {
          std::lock_guard lock(mutex_);
          unavailable_ = true;
          reconnect_at_ = steady_now_() + reconnect_interval_;
          commands_.push_front(std::move(*command_task));
          provisions_.clear();
          Diagnose("数据库命令操作暂不可用");
          continue;
        }
        {
          std::lock_guard lock(mutex_);
          command_operations_.erase(command_task->first);
        }
        EmitCommand({command_task->first, std::move(result)});
        continue;
      }

      if (provision) {
        std::expected<device::DeviceRecord, DatabaseError> result;
        try {
          result = store_.Provision(provision->registration,
                                    provision->received_at);
        } catch (const std::exception&) {
          result = std::unexpected(DatabaseError{
              DatabaseError::Kind::kPermanent, "数据库建档端口异常"});
        } catch (...) {
          result = std::unexpected(DatabaseError{
              DatabaseError::Kind::kPermanent, "数据库建档发生未知异常"});
        }
        {
          std::lock_guard lock(mutex_);
          provisioning_.erase(provision->registration.vendor_id);
          provisions_.erase(provision->registration.vendor_id);
        }
        if (result) {
          Emit({DatabaseResult::Kind::kProvisioned,
                provision->registration.vendor_id, result->revision,
                std::move(*result), {}});
        } else {
          if (result.error().kind == DatabaseError::Kind::kUnavailable) {
            std::lock_guard lock(mutex_);
            unavailable_ = true;
            reconnect_at_ = steady_now_() + reconnect_interval_;
            // 故障前尚未执行的新设备建档全部作废；恢复只依赖 retained replay。
            provisions_.clear();
          }
          const auto kind = result.error().kind == DatabaseError::Kind::kUnavailable
              ? DatabaseResult::Kind::kUnavailable
              : DatabaseResult::Kind::kPermanentFailure;
          const auto context = result.error().kind == DatabaseError::Kind::kUnavailable
              ? kProvisionUnavailable : kProvisionPermanent;
          Emit({kind, provision->registration.vendor_id, 0, std::nullopt,
                context});
          Diagnose(context);
        }
        continue;
      }

      std::expected<void, DatabaseError> result;
      try {
        result = store_.Write(*write);
      } catch (const std::exception&) {
        result = std::unexpected(DatabaseError{
            DatabaseError::Kind::kPermanent, "数据库写入端口异常"});
      } catch (...) {
        result = std::unexpected(DatabaseError{
            DatabaseError::Kind::kPermanent, "数据库写入发生未知异常"});
      }
      if (result) {
        Emit({DatabaseResult::Kind::kWriteCompleted, write->record.vendor_id,
              write->revision, std::nullopt, {}});
      } else {
        if (result.error().kind == DatabaseError::Kind::kUnavailable) {
          std::lock_guard lock(mutex_);
          unavailable_ = true;
          reconnect_at_ = steady_now_() + reconnect_interval_;
          MergeWrite(*write);
          // 任一运行中连接故障都会使故障前新设备候选失效。
          provisions_.clear();
        }
        const auto kind = result.error().kind == DatabaseError::Kind::kUnavailable
            ? DatabaseResult::Kind::kUnavailable
            : DatabaseResult::Kind::kPermanentFailure;
        const auto context = result.error().kind == DatabaseError::Kind::kUnavailable
            ? kWriteUnavailable : kWritePermanent;
        Emit({kind, write->record.vendor_id, write->revision, std::nullopt,
              context});
        Diagnose(context);
      }
    }
  } catch (const std::exception&) {
    Emit({DatabaseResult::Kind::kPermanentFailure, {}, 0, std::nullopt,
          "PostgreSQL 工作线程端口异常"});
    Diagnose("PostgreSQL 工作线程异常退出");
  } catch (...) {
    Emit({DatabaseResult::Kind::kPermanentFailure, {}, 0, std::nullopt,
          "PostgreSQL 工作线程发生未知异常"});
    Diagnose("PostgreSQL 工作线程发生未知异常并退出");
  }
  {
    std::lock_guard lock(mutex_);
    Finish();
  }
}

bool PostgresWorker::FlushAndStop(std::chrono::milliseconds timeout) {
  std::unique_lock lock(mutex_);
  accepting_ = false;
  drain_requested_ = true;
  changed_.notify_all();
  return changed_.wait_until(lock, std::chrono::steady_clock::now() + timeout,
                             [this] { return worker_stopped_; });
}

std::size_t PostgresWorker::PendingDeviceCount() const {
  std::lock_guard lock(mutex_);
  std::unordered_set<std::string> vendors;
  for (const auto& [vendor, task] : provisions_) {
    static_cast<void>(task);
    vendors.insert(vendor);
  }
  for (const auto& [vendor, write] : writes_) {
    static_cast<void>(write);
    vendors.insert(vendor);
  }
  vendors.insert(provisioning_.begin(), provisioning_.end());
  return vendors.size();
}

bool PostgresWorker::IsDatabaseAvailable() const {
  std::lock_guard lock{mutex_};
  return accepting_ && !unavailable_;
}

void PostgresWorker::Emit(DatabaseResult result) noexcept {
  try {
    results_(std::move(result));
  } catch (const std::exception&) {
    Diagnose("数据库结果回调异常");
  } catch (...) {
    Diagnose("数据库结果回调发生未知异常");
  }
}

void PostgresWorker::EmitCommand(CommandDatabaseResult result) noexcept {
  if (!command_results_) return;
  try {
    command_results_(std::move(result));
  } catch (const std::exception&) {
    Diagnose("数据库命令结果回调异常");
  } catch (...) {
    Diagnose("数据库命令结果回调发生未知异常");
  }
}

void PostgresWorker::Diagnose(std::string message) noexcept {
  if (!diagnostic_) return;
  try { diagnostic_(std::move(message)); } catch (...) {}
}

void PostgresWorker::Finish() noexcept {
  // 调用方可能已持锁；worker_stopped_ 只在 Run 线程写，等待方持锁读取。
  worker_stopped_ = true;
  accepting_ = false;
  changed_.notify_all();
}

void PostgresWorker::MergeWrite(persistence::DesiredDeviceWrite write) {
  const auto vendor = write.record.vendor_id;
  const auto it = writes_.find(vendor);
  if (it == writes_.end()) {
    writes_.emplace(vendor, std::move(write));
    return;
  }
  auto& current = it->second;
  current.write_metadata = current.write_metadata || write.write_metadata;
  current.write_status = current.write_status || write.write_status;
  current.write_telemetry = current.write_telemetry || write.write_telemetry;
  if (write.revision >= current.revision) {
    current.record = std::move(write.record);
    current.revision = write.revision;
  }
  if (write.urgency == persistence::Urgency::kImmediate) {
    current.urgency = persistence::Urgency::kImmediate;
  }
}

bool PostgresWorker::HasWork() const {
  return !commands_.empty() || !provisions_.empty() || !writes_.empty();
}
}  // namespace cns::runtime
