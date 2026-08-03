#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>

#include "core/command/command_state.hpp"
#include "core/runtime/device_service.hpp"

namespace cns::runtime {

struct DatabaseError {
  enum class Kind { kUnavailable, kPermanent };
  Kind kind;
  std::string message;
};

struct FindCommandTask { std::string source_id; std::string request_id; };
struct FindCommandByIdTask { std::string command_id; };
struct InsertCommandTask { command::CommandRecord command; };
struct TransitionCommandTask {
  std::string command_id;
  command::CommandStatus expected;
  command::CommandStatus desired;
  command::CommandUpdate update;
};
struct RecoverControlCommandsTask {
  command::TimePoint recovered_at;
  std::size_t limit;
};
struct CleanupCommandsTask {
  command::TimePoint before;
  std::size_t batch_size;
};
using CommandDatabaseTask = std::variant<FindCommandTask, FindCommandByIdTask,
    InsertCommandTask, TransitionCommandTask, RecoverControlCommandsTask,
    CleanupCommandsTask>;
using CommandDatabaseValue = std::variant<std::monostate,
    std::optional<command::CommandRecord>, command::CommandRecord,
    std::vector<command::CommandRecord>, std::size_t>;
struct CommandDatabaseResult {
  std::uint64_t operation_id;
  std::expected<CommandDatabaseValue, DatabaseError> value;
};

class PostgresWorker {
 public:
  class StorePort {
   public:
    virtual ~StorePort() = default;
    virtual std::expected<device::DeviceRecord, DatabaseError> Provision(
        const protocol::Registration&, TimePoint) = 0;
    virtual std::expected<void, DatabaseError> Write(
        const persistence::DesiredDeviceWrite&) = 0;
    virtual std::expected<void, DatabaseError> ReconnectAndValidate() = 0;
    virtual std::expected<std::optional<command::CommandRecord>, DatabaseError>
    FindCommand(std::string_view, std::string_view) {
      return std::unexpected(DatabaseError{DatabaseError::Kind::kPermanent,
                                           "命令查询端口未实现"});
    }
    virtual std::expected<std::optional<command::CommandRecord>, DatabaseError>
    FindCommandById(std::string_view) {
      return std::unexpected(DatabaseError{DatabaseError::Kind::kPermanent,
                                           "命令按ID查询端口未实现"});
    }
    virtual std::expected<command::CommandRecord, DatabaseError> InsertCommand(
        const command::CommandRecord&) {
      return std::unexpected(DatabaseError{DatabaseError::Kind::kPermanent,
                                           "命令写入端口未实现"});
    }
    virtual std::expected<command::CommandRecord, DatabaseError>
    TransitionCommand(std::string_view, command::CommandStatus,
                      command::CommandStatus, const command::CommandUpdate&) {
      return std::unexpected(DatabaseError{DatabaseError::Kind::kPermanent,
                                           "命令转换端口未实现"});
    }
    virtual std::expected<std::size_t, DatabaseError> CleanupCommands(
        command::TimePoint, std::size_t) {
      return std::unexpected(DatabaseError{DatabaseError::Kind::kPermanent,
                                           "命令清理端口未实现"});
    }
    virtual std::expected<std::vector<command::CommandRecord>, DatabaseError>
    RecoverControlCommands(command::TimePoint, std::size_t) {
      return std::unexpected(DatabaseError{
          DatabaseError::Kind::kPermanent, "飞控命令恢复端口未实现"});
    }
  };

  using ResultSink = std::function<void(DatabaseResult)>;
  using ReplayRequest = std::function<void()>;
  using SteadyNow = std::function<std::chrono::steady_clock::time_point()>;
  using DiagnosticSink = std::function<void(std::string)>;
  using CommandResultSink = std::function<void(CommandDatabaseResult)>;

  PostgresWorker(StorePort& store, ResultSink results, ReplayRequest replay,
                 SteadyNow steady_now, DiagnosticSink diagnostic = {},
                 std::chrono::seconds reconnect_interval =
                     std::chrono::seconds{5},
                 CommandResultSink command_results = {},
                 std::size_t max_inflight_commands = 256);

  // 接纳尚未在途的 device_id；关闭、数据库不可用或同 device_id 已在途时返回 false。
  // latest registration 只由 DeviceService 的 pending 状态合并和持有。
  bool SubmitProvision(protocol::Registration registration, TimePoint at);
  bool SubmitWrite(persistence::DesiredDeviceWrite write);
  bool SubmitCommand(std::uint64_t operation_id, CommandDatabaseTask task);
  // 唯一允许调用 StorePort 的入口。
  void Run(std::stop_token stop);
  // 只请求 Run 线程排空并等待，不调用 StorePort。
  bool FlushAndStop(std::chrono::milliseconds timeout);
  [[nodiscard]] std::size_t PendingDeviceCount() const;
  [[nodiscard]] bool IsDatabaseAvailable() const;

 private:
  struct ProvisionTask {
    protocol::Registration registration;
    TimePoint received_at;
  };

  void Emit(DatabaseResult result) noexcept;
  void EmitCommand(CommandDatabaseResult result) noexcept;
  void Diagnose(std::string message) noexcept;
  void Finish() noexcept;
  void MergeWrite(persistence::DesiredDeviceWrite write);
  bool HasWork() const;

  StorePort& store_;
  ResultSink results_;
  ReplayRequest replay_;
  SteadyNow steady_now_;
  DiagnosticSink diagnostic_;
  CommandResultSink command_results_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::unordered_map<std::string, ProvisionTask> provisions_;
  std::unordered_map<std::string, persistence::DesiredDeviceWrite> writes_;
  std::unordered_set<std::string> provisioning_;
  std::deque<std::pair<std::uint64_t, CommandDatabaseTask>> commands_;
  std::unordered_set<std::uint64_t> command_operations_;
  std::size_t max_inflight_commands_;
  bool unavailable_ = false;
  bool accepting_ = true;
  bool drain_requested_ = false;
  bool worker_stopped_ = false;
  std::chrono::steady_clock::time_point reconnect_at_{};
  std::chrono::seconds reconnect_interval_;
};

}  // namespace cns::runtime
