// 本文件实现配置命令的受理、幂等与异步端口编排。
#include "core/runtime/command_service.hpp"

#include <algorithm>
#include <utility>
#include <thread>
#include <ranges>

#include "core/command/config_policy.hpp"

namespace cns::runtime {
namespace {

command::ProtocolError Error(std::string code, std::string message) {
  return {std::move(code), std::move(message)};
}

std::string SourceAckTopic(std::string_view topic_namespace,
                           std::string_view source_id) {
  return std::string{topic_namespace} + "/sources/" + std::string{source_id} +
         "/config/ack";
}

std::string DeviceSetTopic(std::string_view topic_namespace,
                           std::string_view vendor_id) {
  return std::string{topic_namespace} + "/" + std::string{vendor_id} +
         "/config/set";
}

}  // namespace

CommandService::CommandService(
    command::SourceCatalog& sources, device::DeviceRegistry& devices,
    DatabaseSubmitter database_submitter, DevicePublisher device_publisher,
    SourceAckPublisher source_ack_publisher, DiagnosticSink diagnostic,
    std::size_t max_inflight_commands, std::string topic_namespace,
    std::chrono::seconds config_timeout, std::chrono::days terminal_retention,
    std::chrono::seconds cleanup_interval, std::size_t cleanup_batch_size)
    : sources_(sources),
      devices_(devices),
      database_submitter_(std::move(database_submitter)),
      device_publisher_(std::move(device_publisher)),
      source_ack_publisher_(std::move(source_ack_publisher)),
      diagnostic_(std::move(diagnostic)),
      topic_namespace_(std::move(topic_namespace)),
      max_inflight_commands_(max_inflight_commands),
      config_timeout_(config_timeout),
      terminal_retention_(terminal_retention),
      cleanup_interval_(cleanup_interval),
      cleanup_batch_size_(cleanup_batch_size) {}

bool CommandService::TryPush(mqtt::InboundMessage message) {
  std::lock_guard lock{input_mutex_};
  if (closed_ || messages_.size() >= max_inflight_commands_) return false;
  messages_.push_back(std::move(message));
  return true;
}

void CommandService::PushDatabaseResult(CommandDatabaseResult result) {
  std::lock_guard lock{input_mutex_};
  if (!closed_) database_results_.push_back(std::move(result));
}

void CommandService::PushPublishCompletion(mqtt::PublishCompletion completion) {
  std::lock_guard lock{input_mutex_};
  if (!closed_) publish_completions_.push_back(std::move(completion));
}

void CommandService::ProcessReady(command::TimePoint now) {
  for (;;) {
    std::optional<CommandDatabaseResult> database_result;
    std::optional<mqtt::PublishCompletion> publish_completion;
    std::optional<mqtt::InboundMessage> message;
    {
      std::lock_guard lock{input_mutex_};
      if (!database_results_.empty()) {
        database_result = std::move(database_results_.front());
        database_results_.pop_front();
      } else if (!publish_completions_.empty()) {
        publish_completion = std::move(publish_completions_.front());
        publish_completions_.pop_front();
      } else if (!messages_.empty()) {
        message = std::move(messages_.front());
        messages_.pop_front();
      } else {
        break;
      }
    }
    if (database_result) Handle(std::move(*database_result), now);
    if (publish_completion) Handle(std::move(*publish_completion), now);
    if (message) Handle(std::move(*message), now);
  }
  ProcessTimeoutsAndRecovery(now);
}

void CommandService::Close() {
  std::lock_guard lock{input_mutex_};
  closed_ = true;
}

void CommandService::CancelOutstandingWork() {
  std::lock_guard lock{input_mutex_};
  messages_.clear();
  database_results_.clear();
  publish_completions_.clear();
  operations_.clear();
  publications_.clear();
}

void CommandService::SetDatabaseAvailable(bool available) {
  database_available_ = available;
}

void CommandService::SetMqttAvailable(bool available) {
  mqtt_available_ = available;
  if (available) recovery_started_.clear();
}

void CommandService::LoadActive(std::vector<command::CommandRecord> commands) {
  if (commands.size() > max_inflight_commands_) {
    Diagnose("活动命令数量超过配置上限");
    commands.resize(max_inflight_commands_);
  }
  for (auto& record : commands) {
    const auto* source = sources_.Find(record.source_id);
    if (source == nullptr || command::IsTerminal(record.status)) continue;
    auto parsed = command::ParseSourceConfigRequest(record.request_payload.dump(),
                                                     source->kind);
    RequestContext context{record.source_id, source, std::move(parsed),
                           record.created_at, TargetFor(record), record};
    active_commands_.insert_or_assign(record.command_id, std::move(context));
  }
}

void CommandService::OnTargetOnline(std::string_view vendor_id,
                                    command::TimePoint now) {
  for (auto& [command_id, context] : active_commands_) {
    if (context.record && context.record->target_vendor_id == vendor_id) {
      recovery_started_.erase(command_id);
    }
  }
  ProcessTimeoutsAndRecovery(now);
}

bool CommandService::WaitForDatabaseIdle(std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!operations_.empty() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  return operations_.empty();
}

std::size_t CommandService::ActiveCommandCount() const {
  return active_commands_.size();
}

void CommandService::SetDiagnosticSinkForTesting(DiagnosticSink diagnostic) {
  diagnostic_ = std::move(diagnostic);
}

void CommandService::Handle(mqtt::InboundMessage message,
                            command::TimePoint now) {
  if (const auto vendor =
          command::ParseDeviceConfigAckTopic(topic_namespace_, message.topic)) {
    HandleDeviceAck(*vendor, message.payload, now);
    return;
  }
  const auto source_id =
      command::ParseSourceRequestTopic(topic_namespace_, message.topic);
  if (!source_id) return;
  const auto* source = sources_.Find(*source_id);
  if (source == nullptr) {
    Reject(*source_id, std::nullopt,
           Error("source_not_registered", "命令来源未登记"), now);
    return;
  }
  if (!source->enabled) {
    Reject(*source_id, std::nullopt,
           Error("source_disabled", "命令来源已禁用"), now);
    return;
  }
  if (source->kind == command::SourceKind::kDevice) {
    const auto* source_device =
        source->device_vendor_id ? devices_.Find(*source->device_vendor_id) : nullptr;
    if (source_device == nullptr ||
        source_device->status != device::Status::kOnline) {
      Reject(*source_id, std::nullopt,
             Error("source_device_offline", "来源设备当前离线"), now);
      return;
    }
  }
  const auto parsed =
      command::ParseSourceConfigRequest(message.payload, source->kind);
  const auto* accepted = std::get_if<command::SourceConfigRequest>(&parsed);
  const auto* rejected = std::get_if<command::RejectedSourceRequest>(&parsed);
  const auto request_id = accepted ? std::optional<std::string>{accepted->request_id}
                                   : rejected->request_id;
  if (!request_id) {
    Reject(*source_id, std::nullopt, rejected->error, now);
    return;
  }
  if (!mqtt_available_) {
    Reject(*source_id, *request_id,
           Error("mqtt_unavailable", "MQTT当前不可用"), now);
    return;
  }
  if (!database_available_) {
    Reject(*source_id, *request_id,
           Error("database_unavailable", "数据库当前不可用"), now);
    return;
  }
  if (operations_.size() + publications_.size() >= max_inflight_commands_) {
    Reject(*source_id, *request_id, Error("server_busy", "服务器命令容量已满"),
           now);
    return;
  }
  RequestContext context{.source_id = *source_id,
                         .source = source,
                         .parsed = parsed,
                         .received_at = message.received_at,
                         .target = std::nullopt,
                         .record = std::nullopt};
  if (!Submit(OperationKind::kFind, std::move(context),
              FindCommandTask{*source_id, *request_id})) {
    Reject(*source_id, *request_id,
           Error("database_unavailable", "数据库当前不可用"), now);
  }
}

void CommandService::Handle(CommandDatabaseResult result,
                            command::TimePoint now) {
  const auto found = operations_.find(result.operation_id);
  if (found == operations_.end()) return;
  auto operation = std::move(found->second);
  operations_.erase(found);
  if (!result.value) {
    if (result.value.error().kind == DatabaseError::Kind::kUnavailable) {
      database_available_ = false;
    }
    Diagnose("命令数据库操作失败");
    return;
  }

  if (operation.kind == OperationKind::kCleanup) {
    if (std::get_if<std::size_t>(&*result.value) == nullptr) {
      Diagnose("命令清理返回类型异常");
    }
    return;
  }

  if (operation.kind == OperationKind::kFind) {
    const auto* existing_value =
        std::get_if<std::optional<command::CommandRecord>>(&*result.value);
    if (existing_value == nullptr) {
      Diagnose("命令查询返回类型异常");
      return;
    }
    const auto comparison = std::visit(
        [](const auto& parsed) -> std::optional<nlohmann::json> {
          if constexpr (std::is_same_v<std::decay_t<decltype(parsed)>,
                                       command::SourceConfigRequest>) {
            return std::optional<nlohmann::json>{parsed.comparison_payload};
          } else {
            return parsed.comparison_payload;
          }
        },
        operation.context.parsed);
    if (*existing_value) {
      const auto& existing = **existing_value;
      if (!comparison || command::CompareRequest(existing, *comparison) ==
                             command::IdempotencyResult::kConflict) {
        const auto request_id = std::visit(
            [](const auto& parsed) -> std::optional<std::string> {
              if constexpr (std::is_same_v<std::decay_t<decltype(parsed)>,
                                           command::SourceConfigRequest>) {
                return parsed.request_id;
              } else {
                return parsed.request_id;
              }
            },
            operation.context.parsed);
        Reject(operation.context.source_id, *request_id,
               Error("idempotency_conflict", "同一请求号的内容不一致"), now);
        return;
      }
      operation.context.record = existing;
      operation.context.target = TargetFor(existing);
      PublishAck(operation.context, now);
      return;
    }

    command::CommandRecord record{
        .command_id = command::GenerateUuidV4(),
        .source_id = operation.context.source_id,
        .request_id = std::visit(
            [](const auto& parsed) {
              if constexpr (std::is_same_v<std::decay_t<decltype(parsed)>,
                                           command::SourceConfigRequest>) {
                return parsed.request_id;
              } else {
                return *parsed.request_id;
              }
            },
            operation.context.parsed),
        .target_vendor_id = std::nullopt,
        .request_payload = *comparison,
        .status = command::CommandStatus::kPending,
        .error_code = std::nullopt,
        .error_message = std::nullopt,
        .device_ack = std::nullopt,
        .created_at = operation.context.received_at,
        .dispatched_at = std::nullopt,
        .updated_at = operation.context.received_at,
        .completed_at = std::nullopt};
    if (const auto* rejected =
            std::get_if<command::RejectedSourceRequest>(&operation.context.parsed)) {
      record.status = command::CommandStatus::kFailed;
      record.error_code = rejected->error.code;
      record.error_message = rejected->error.message;
      record.completed_at = operation.context.received_at;
    } else {
      const auto& request =
          std::get<command::SourceConfigRequest>(operation.context.parsed);
      const auto target = command::ResolveConfigTarget(
          *operation.context.source, request, devices_);
      if (!target) {
        record.status = command::CommandStatus::kFailed;
        record.error_code = target.error().code;
        record.error_message = target.error().message;
        record.completed_at = operation.context.received_at;
      } else {
        operation.context.target = *target;
        record.target_vendor_id = target->vendor_id;
        const auto* target_device = devices_.Find(target->vendor_id);
        if (target_device == nullptr ||
            target_device->status != device::Status::kOnline) {
          record.status = command::CommandStatus::kFailed;
          record.error_code = "target_offline";
          record.error_message = "目标设备当前离线";
          record.completed_at = operation.context.received_at;
        }
      }
    }
    operation.context.record = record;
    static_cast<void>(Submit(OperationKind::kInsert, std::move(operation.context),
                             InsertCommandTask{std::move(record)}));
    return;
  }

  const auto* record = std::get_if<command::CommandRecord>(&*result.value);
  if (record == nullptr) {
    Diagnose("命令写入返回类型异常");
    return;
  }
  if (operation.kind == OperationKind::kInsert && operation.context.record &&
      operation.context.record->command_id != record->command_id) {
    if (command::CompareRequest(*record,
                                operation.context.record->request_payload) ==
        command::IdempotencyResult::kConflict) {
      Reject(operation.context.source_id, operation.context.record->request_id,
             Error("idempotency_conflict", "同一请求号的内容不一致"), now);
      return;
    }
    operation.context.record = *record;
    operation.context.target = TargetFor(*record);
    PublishAck(operation.context, now);
    return;
  }
  operation.context.record = *record;
  if (operation.kind == OperationKind::kTransition ||
      command::IsTerminal(record->status)) {
    PublishAck(operation.context, now);
    if (command::IsTerminal(record->status)) {
      active_commands_.erase(record->command_id);
      recovery_started_.erase(record->command_id);
    } else {
      active_commands_.insert_or_assign(record->command_id, operation.context);
    }
    return;
  }
  active_commands_.insert_or_assign(record->command_id, operation.context);
  PublishRecord(std::move(operation.context), now);
}

void CommandService::PublishRecord(RequestContext context,
                                   command::TimePoint now) {
  if (!context.record || !context.record->target_vendor_id || !mqtt_available_) return;
  const auto* request = std::get_if<command::SourceConfigRequest>(&context.parsed);
  if (request == nullptr) return;
  const auto previous_status = context.record->status;
  recovery_started_.insert(context.record->command_id);
  const auto token = next_publish_token_++;
  publications_.emplace(token, PublishedCommand{context});
  const auto payload =
      command::BuildDeviceConfigSet(context.record->command_id, request->parameters).dump();
  const auto published = device_publisher_(
      token, DeviceSetTopic(topic_namespace_, *context.record->target_vendor_id), payload);
  if (!published) {
    publications_.erase(token);
    auto failed = *context.record;
    failed.status = command::CommandStatus::kFailed;
    failed.error_code = "mqtt_publish_failed";
    failed.error_message = "设备配置命令发布失败";
    failed.updated_at = now;
    failed.completed_at = now;
    context.record = failed;
    const command::CommandUpdate update{
        .error_code = failed.error_code,
        .error_message = failed.error_message,
        .device_ack = std::nullopt,
        .dispatched_at = std::nullopt,
        .completed_at = now,
        .updated_at = now};
    static_cast<void>(Submit(
        OperationKind::kTransition, std::move(context),
        TransitionCommandTask{failed.command_id, previous_status,
                              command::CommandStatus::kFailed, update}));
  }
}

void CommandService::HandleDeviceAck(std::string_view vendor_id,
                                     std::string_view payload,
                                     command::TimePoint now) {
  const auto parsed = command::ParseDeviceConfigAck(payload);
  if (!parsed) { Diagnose("设备配置ACK格式非法"); return; }
  const auto found = active_commands_.find(parsed->command_id);
  if (found == active_commands_.end() || !found->second.record ||
      found->second.record->target_vendor_id != vendor_id) {
    Diagnose("设备配置ACK无法关联活动命令");
    return;
  }
  auto context = found->second;
  const auto current = context.record->status;
  if (current != command::CommandStatus::kPending &&
      current != command::CommandStatus::kDispatched) return;
  const auto desired = parsed->business_status == "rejected"
                           ? command::CommandStatus::kFailed
                           : command::CommandStatus::kSucceeded;
  auto record = *context.record;
  record.status = desired;
  record.device_ack = parsed->raw;
  record.updated_at = now;
  record.completed_at = now;
  if (desired == command::CommandStatus::kFailed) {
    record.error_code = "device_rejected";
    record.error_message = "设备拒绝配置命令";
  }
  context.record = record;
  command::CommandUpdate update{record.error_code, record.error_message,
                                record.device_ack, std::nullopt, now, now};
  static_cast<void>(Submit(OperationKind::kTransition, std::move(context),
      TransitionCommandTask{record.command_id, current, desired, update}));
}

void CommandService::ProcessTimeoutsAndRecovery(command::TimePoint now) {
  if (!next_cleanup_at_) {
    next_cleanup_at_ = now + cleanup_interval_;
  } else if (now >= *next_cleanup_at_) {
    const bool cleanup_pending = std::ranges::any_of(
        operations_, [](const auto& item) {
          return item.second.kind == OperationKind::kCleanup;
        });
    if (!cleanup_pending && database_available_) {
      RequestContext context{
          .source_id = {}, .source = nullptr,
          .parsed = command::RejectedSourceRequest{
              std::nullopt, std::nullopt, {"cleanup", "终态清理"}},
          .received_at = now, .target = std::nullopt, .record = std::nullopt};
      static_cast<void>(Submit(OperationKind::kCleanup, std::move(context),
          CleanupCommandsTask{now - terminal_retention_, cleanup_batch_size_}));
    }
    next_cleanup_at_ = now + cleanup_interval_;
  }
  std::vector<std::string> ids;
  ids.reserve(active_commands_.size());
  for (const auto& [id, unused] : active_commands_) { (void)unused; ids.push_back(id); }
  for (const auto& id : ids) {
    const auto found = active_commands_.find(id);
    if (found == active_commands_.end() || !found->second.record) continue;
    auto context = found->second;
    const auto record = *context.record;
    if (now >= record.created_at + config_timeout_) {
      if (recovery_started_.insert(id).second) {
        auto timed_out = record;
        timed_out.status = command::CommandStatus::kTimeout;
        timed_out.error_code = "command_timeout";
        timed_out.error_message = "设备配置命令超时";
        timed_out.updated_at = now;
        timed_out.completed_at = now;
        context.record = timed_out;
        command::CommandUpdate update{timed_out.error_code, timed_out.error_message,
                                      std::nullopt, std::nullopt, now, now};
        static_cast<void>(Submit(OperationKind::kTransition, std::move(context),
            TransitionCommandTask{id, record.status,
                                  command::CommandStatus::kTimeout, update}));
      }
      continue;
    }
    if (!mqtt_available_ || recovery_started_.contains(id) ||
        !record.target_vendor_id) continue;
    const auto* target = devices_.Find(*record.target_vendor_id);
    if (target == nullptr || target->status != device::Status::kOnline) continue;
    recovery_started_.insert(id);
    PublishRecord(std::move(context), now);
  }
}

void CommandService::Handle(mqtt::PublishCompletion completion,
                            command::TimePoint now) {
  const auto found = publications_.find(completion.token);
  if (found == publications_.end()) return;
  auto context = std::move(found->second.context);
  publications_.erase(found);
  auto record = *context.record;
  const auto desired = completion.result ? command::CommandStatus::kDispatched
                                         : command::CommandStatus::kFailed;
  record.status = desired;
  record.updated_at = now;
  command::CommandUpdate update{.error_code = std::nullopt,
                                .error_message = std::nullopt,
                                .device_ack = std::nullopt,
                                .dispatched_at = std::nullopt,
                                .completed_at = std::nullopt,
                                .updated_at = now};
  if (completion.result) {
    record.dispatched_at = now;
    update.dispatched_at = now;
  } else {
    record.error_code = "mqtt_publish_failed";
    record.error_message = "设备配置命令发布失败";
    record.completed_at = now;
    update.error_code = record.error_code;
    update.error_message = record.error_message;
    update.completed_at = now;
  }
  context.record = record;
  static_cast<void>(Submit(
      OperationKind::kTransition, std::move(context),
      TransitionCommandTask{record.command_id, command::CommandStatus::kPending,
                            desired, update}));
}

bool CommandService::Submit(OperationKind kind, RequestContext context,
                            CommandDatabaseTask task) {
  const auto operation_id = next_operation_id_++;
  if (!database_submitter_(operation_id, std::move(task))) return false;
  operations_.emplace(operation_id,
                      Operation{kind, std::move(context)});
  return true;
}

void CommandService::Reject(std::string_view source_id,
                            std::optional<std::string_view> request_id,
                            command::ProtocolError error,
                            command::TimePoint now) {
  try {
    source_ack_publisher_(
        SourceAckTopic(topic_namespace_, source_id),
        command::BuildPrePersistenceRejection(request_id, std::move(error), now)
            .dump());
  } catch (...) {
    Diagnose("来源配置ACK发布回调异常");
  }
}

void CommandService::PublishAck(const RequestContext& context,
                                command::TimePoint now) {
  if (!context.record) return;
  try {
    source_ack_publisher_(SourceAckTopic(topic_namespace_, context.source_id),
                          command::BuildSourceAck(*context.record, context.target,
                                                  now)
                              .dump());
  } catch (...) {
    Diagnose("来源配置ACK发布回调异常");
  }
}

void CommandService::Diagnose(std::string message) noexcept {
  try {
    if (diagnostic_) diagnostic_(std::move(message));
  } catch (...) {
  }
}

std::optional<command::ResolvedTarget> CommandService::TargetFor(
    const command::CommandRecord& record) const {
  if (!record.target_vendor_id) return std::nullopt;
  const auto* target = devices_.Find(*record.target_vendor_id);
  if (target == nullptr) return std::nullopt;
  return command::ResolvedTarget{target->vendor_id, target->school_name,
                                 target->dcdw_label};
}

}  // namespace cns::runtime
