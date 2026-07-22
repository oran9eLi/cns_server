// 本文件声明配置命令的单线程业务编排器。
#pragma once

#include <cstdint>
#include <atomic>
#include <deque>
#include <expected>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "adapters/mqtt/mqtt_client.hpp"
#include "core/command/command_state.hpp"
#include "core/command/source_request.hpp"
#include "core/runtime/postgres_worker.hpp"

namespace cns::runtime {

class CommandService {
 public:
  using DatabaseSubmitter =
      std::function<bool(std::uint64_t, CommandDatabaseTask)>;
  using DevicePublisher = std::function<std::expected<void, std::string>(
      std::uint64_t, std::string, std::string)>;
  using SourceAckPublisher = std::function<void(std::string, std::string)>;
  using DiagnosticSink = std::function<void(std::string)>;
  using InformationSink = std::function<void(std::string)>;

  CommandService(command::SourceCatalog& sources,
                 device::DeviceRegistry& devices,
                 DatabaseSubmitter database_submitter,
                 DevicePublisher device_publisher,
                 SourceAckPublisher source_ack_publisher,
                 DiagnosticSink diagnostic = {},
                 std::size_t max_inflight_commands = 256,
                 std::string topic_namespace = "cns",
                 std::chrono::seconds config_timeout = std::chrono::seconds{15},
                 std::chrono::seconds control_timeout = std::chrono::seconds{30},
                 std::chrono::days terminal_retention = std::chrono::days{30},
                 std::chrono::seconds cleanup_interval = std::chrono::seconds{3600},
                 std::size_t cleanup_batch_size = 100,
                 InformationSink information = {});

  bool TryPush(mqtt::InboundMessage message);
  void PushDatabaseResult(CommandDatabaseResult result);
  void PushPublishCompletion(mqtt::PublishCompletion completion);
  void ProcessReady(command::TimePoint now);
  void Close();
  void CancelOutstandingWork();
  void SetDatabaseAvailable(bool available);
  void SetMqttAvailable(bool available);
  void LoadActive(std::vector<command::CommandRecord> commands);
  void OnTargetOnline(std::string_view vendor_id, command::TimePoint now);
  bool WaitForDatabaseIdle(std::chrono::milliseconds timeout);
  [[nodiscard]] std::size_t ActiveCommandCount() const;
  void SetDiagnosticSinkForTesting(DiagnosticSink diagnostic);

 private:
  struct RequestContext {
    std::string source_id;
    const command::CommandSource* source = nullptr;
    command::SourceRequestParseResult parsed;
    command::CommandType command_type = command::CommandType::kConfig;
    command::TimePoint received_at;
    command::TimePoint deadline;
    std::optional<command::ResolvedTarget> target;
    std::optional<command::CommandRecord> record;
    std::optional<std::string> device_ack_business_status;
  };

  enum class OperationKind {
    kFind,
    kFindByIdForConvergence,
    kInsert,
    kTransition,
    kCleanup
  };
  struct Operation {
    OperationKind kind;
    RequestContext context;
  };
  struct PublishedCommand {
    RequestContext context;
  };
  struct DeferredTransition {
    RequestContext context;
    TransitionCommandTask task;
  };
  struct CompletedCommand {
    RequestContext context;
    command::TimePoint remembered_at;
  };
  struct EarlyDeviceAck {
    std::string vendor_id;
    std::string payload;
    command::TimePoint received_at;
  };

  void Handle(mqtt::InboundMessage message, command::TimePoint now);
  void Handle(CommandDatabaseResult result, command::TimePoint now);
  void Handle(mqtt::PublishCompletion completion, command::TimePoint now);
  void HandleDeviceAck(std::string_view vendor_id, std::string_view payload,
                       command::CommandType command_type,
                       command::TimePoint now);
  void ProcessTimeoutsAndRecovery(command::TimePoint now);
  void PublishRecord(RequestContext context, command::TimePoint now);
  bool Submit(OperationKind kind, RequestContext context,
              CommandDatabaseTask task);
  void SubmitTransitionOrDefer(RequestContext context,
                               TransitionCommandTask task);
  void RetryDeferredTransitions();
  void HandleTransitionConflict(Operation operation,
                                command::TimePoint now);
  void ConvergeAfterConflict(Operation operation,
                             const command::CommandRecord& current,
                             command::TimePoint now);
  void RememberCompletedCommand(RequestContext context, command::TimePoint now);
  bool TryHandleLateDeviceAck(std::string_view vendor_id,
                              command::CommandType command_type,
                              std::string_view command_id,
                              std::string_view business_status,
                              command::TimePoint now);
  bool IsStateChangedError(const DatabaseError& error) const;
  void Reject(std::string_view source_id,
              std::optional<std::string_view> request_id,
              command::ProtocolError error, command::TimePoint now,
              command::CommandType command_type = command::CommandType::kConfig);
  void PublishAck(const RequestContext& context, command::TimePoint now);
  void Diagnose(std::string message) noexcept;
  void Inform(std::string message) noexcept;
  std::optional<command::ResolvedTarget> TargetFor(
      const command::CommandRecord& record) const;

  command::SourceCatalog& sources_;
  device::DeviceRegistry& devices_;
  DatabaseSubmitter database_submitter_;
  DevicePublisher device_publisher_;
  SourceAckPublisher source_ack_publisher_;
  DiagnosticSink diagnostic_;
  InformationSink information_;
  std::string topic_namespace_;
  std::size_t max_inflight_commands_;
  std::uint64_t next_operation_id_ = 1;
  std::uint64_t next_publish_token_ = 1;
  std::deque<mqtt::InboundMessage> messages_;
  std::deque<CommandDatabaseResult> database_results_;
  std::deque<mqtt::PublishCompletion> publish_completions_;
  std::unordered_map<std::uint64_t, Operation> operations_;
  std::unordered_map<std::uint64_t, PublishedCommand> publications_;
  std::unordered_map<std::string, RequestContext> active_commands_;
  std::unordered_map<std::string, CompletedCommand> completed_commands_;
  std::unordered_set<std::string> recovery_started_;
  std::unordered_map<std::string, DeferredTransition> deferred_transitions_;
  std::unordered_map<std::string, EarlyDeviceAck> early_device_acks_;
  std::mutex input_mutex_;
  std::chrono::seconds config_timeout_;
  std::chrono::seconds control_timeout_;
  std::chrono::days terminal_retention_;
  std::chrono::seconds cleanup_interval_;
  std::size_t cleanup_batch_size_;
  std::optional<command::TimePoint> next_cleanup_at_;
  std::atomic_size_t outstanding_operations_{0};
  std::atomic_bool database_available_{true};
  std::atomic_bool mqtt_available_{true};
  bool closed_ = false;
};

}  // namespace cns::runtime
