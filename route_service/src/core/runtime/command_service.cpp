// 本文件实现配置命令的受理、幂等与异步端口编排。
#include "core/runtime/command_service.hpp"

#include <algorithm>
#include <array>
#include <sstream>
#include <utility>
#include <thread>
#include <ranges>

#include "core/command/config_policy.hpp"
#include "core/command/control_policy.hpp"

namespace cns::runtime {
namespace {

command::ProtocolError Error(std::string code, std::string message) {
  return {std::move(code), std::move(message)};
}

std::string SourceAckTopic(std::string_view topic_namespace,
                           std::string_view source_id,
                           command::CommandType type) {
  return std::string{topic_namespace} + "/sources/" + std::string{source_id} +
         (type == command::CommandType::kControl ? "/control/ack" : "/config/ack");
}

std::string DeviceSetTopic(std::string_view topic_namespace,
                           std::string_view vendor_id,
                           command::CommandType type) {
  return std::string{topic_namespace} + "/" + std::string{vendor_id} +
         (type == command::CommandType::kControl ? "/control/set" : "/config/set");
}

std::optional<std::string> RequestId(
    const command::SourceRequestParseResult& parsed) {
  return std::visit([](const auto& value) -> std::optional<std::string> {
    if constexpr (std::is_same_v<std::decay_t<decltype(value)>,
                                 command::RejectedSourceRequest>) {
      return value.request_id;
    } else {
      return value.request_id;
    }
  }, parsed);
}

std::optional<nlohmann::json> ComparisonPayload(
    const command::SourceRequestParseResult& parsed) {
  return std::visit([](const auto& value) -> std::optional<nlohmann::json> {
    if constexpr (std::is_same_v<std::decay_t<decltype(value)>,
                                 command::RejectedSourceRequest>) {
      return value.comparison_payload;
    } else {
      return std::optional<nlohmann::json>{value.comparison_payload};
    }
  }, parsed);
}

std::string DescribeTarget(const command::ResolvedTarget& target) {
  const auto school = target.school_name.empty() ? "未知学校" : target.school_name;
  const auto label = target.dcdw_label && !target.dcdw_label->empty()
                         ? *target.dcdw_label
                         : "未登记编号";
  return school + " / " + label + "（" + target.vendor_id + "）";
}

void AppendConfigField(std::ostringstream& output, bool& first,
                       std::string_view name,
                       const std::optional<std::uint32_t>& value) {
  if (!value) return;
  if (!first) output << "，";
  output << name << '=' << *value;
  first = false;
}

std::string DescribeConfigCommand(const command::ConfigParameters& parameters) {
  std::ostringstream output;
  bool first = true;
  AppendConfigField(output, first, "telemetry_publish_interval_ms",
                    parameters.telemetry_publish_interval_ms);
  AppendConfigField(output, first, "heartbeat_interval_ms",
                    parameters.heartbeat_interval_ms);
  AppendConfigField(output, first, "mqtt_reconnect_delay_s",
                    parameters.mqtt_reconnect_delay_s);
  AppendConfigField(output, first, "mqtt_reconnect_delay_max_s",
                    parameters.mqtt_reconnect_delay_max_s);
  return output.str();
}

std::string_view ControlCommandName(command::ControlCommand command) noexcept {
  switch (command) {
    case command::ControlCommand::kSetMotorPwm:
      return "set_motor_pwm";
    case command::ControlCommand::kEmergencyStop:
      return "emergency_stop";
    case command::ControlCommand::kTakeoff:
      return "takeoff";
    case command::ControlCommand::kLand:
      return "land";
  }
  return "unknown";
}

std::string DescribeControlCommand(const command::SourceControlRequest& request) {
  std::ostringstream output;
  output << ControlCommandName(request.command);
  if (request.command == command::ControlCommand::kSetMotorPwm &&
      request.parameters.pwm_us) {
    const auto& pwm = *request.parameters.pwm_us;
    output << " pwm_us=[" << pwm[0] << ',' << pwm[1] << ',' << pwm[2] << ','
           << pwm[3] << ']';
  }
  return output.str();
}

std::string DescribeCommand(
    const command::SourceRequestParseResult& parsed) {
  if (const auto* request =
          std::get_if<command::SourceConfigRequest>(&parsed)) {
    return "配置命令 " + DescribeConfigCommand(request->parameters);
  }
  if (const auto* request =
          std::get_if<command::SourceControlRequest>(&parsed)) {
    return "飞控命令 " + DescribeControlCommand(*request);
  }
  return "未知命令";
}

}  // namespace

CommandService::CommandService(
    command::SourceCatalog& sources, device::DeviceRegistry& devices,
    DatabaseSubmitter database_submitter, DevicePublisher device_publisher,
    SourceAckPublisher source_ack_publisher, DiagnosticSink diagnostic,
    std::size_t max_inflight_commands, std::string topic_namespace,
    std::chrono::seconds config_timeout, std::chrono::seconds control_timeout,
    std::chrono::days terminal_retention,
    std::chrono::seconds cleanup_interval, std::size_t cleanup_batch_size,
    InformationSink information)
    : sources_(sources),
      devices_(devices),
      database_submitter_(std::move(database_submitter)),
      device_publisher_(std::move(device_publisher)),
      source_ack_publisher_(std::move(source_ack_publisher)),
      diagnostic_(std::move(diagnostic)),
      information_(std::move(information)),
      topic_namespace_(std::move(topic_namespace)),
      max_inflight_commands_(max_inflight_commands),
      config_timeout_(config_timeout),
      control_timeout_(control_timeout),
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
  RetryDeferredTransitions();
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
  outstanding_operations_.store(0, std::memory_order_release);
  publications_.clear();
  deferred_transitions_.clear();
  early_device_acks_.clear();
}

void CommandService::SetDatabaseAvailable(bool available) {
  database_available_ = available;
}

void CommandService::SetMqttAvailable(bool available) {
  const bool was_available =
      mqtt_available_.exchange(available, std::memory_order_acq_rel);
  if (available && !was_available) recovery_started_.clear();
}

void CommandService::LoadActive(std::vector<command::CommandRecord> commands) {
  if (commands.size() > max_inflight_commands_) {
    Diagnose("活动命令数量超过配置上限");
    return;
  }
  for (auto& record : commands) {
    const auto* source = sources_.Find(record.source_id);
    if (source == nullptr || command::IsTerminal(record.status)) continue;
    auto parsed = record.command_type == command::CommandType::kControl
        ? command::ParseSourceControlRequest(record.request_payload.dump(), source->kind)
        : command::ParseSourceConfigRequest(record.request_payload.dump(), source->kind);
    const auto timeout = record.command_type == command::CommandType::kControl
                             ? control_timeout_ : config_timeout_;
    const auto base = record.status == command::CommandStatus::kInProgress
                          ? record.updated_at : record.created_at;
    RequestContext context{record.source_id, source, std::move(parsed),
                           record.command_type, record.created_at,
                           base + timeout, TargetFor(record), record,
                           std::nullopt};
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
  while (outstanding_operations_.load(std::memory_order_acquire) != 0 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  return outstanding_operations_.load(std::memory_order_acquire) == 0;
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
    HandleDeviceAck(*vendor, message.payload, command::CommandType::kConfig, now);
    return;
  }
  if (const auto vendor =
          command::ParseDeviceControlAckTopic(topic_namespace_, message.topic)) {
    HandleDeviceAck(*vendor, message.payload, command::CommandType::kControl, now);
    return;
  }
  auto command_type = command::CommandType::kConfig;
  auto source_id = command::ParseSourceRequestTopic(topic_namespace_, message.topic);
  if (!source_id) {
    source_id = command::ParseSourceControlRequestTopic(topic_namespace_, message.topic);
    command_type = command::CommandType::kControl;
  }
  if (!source_id) return;
  const auto* source = sources_.Find(*source_id);
  if (source == nullptr) {
    Reject(*source_id, std::nullopt,
           Error("source_not_registered", "命令来源未登记"), now,
           command_type);
    return;
  }
  if (!source->enabled) {
    Reject(*source_id, std::nullopt,
           Error("source_disabled", "命令来源已禁用"), now, command_type);
    return;
  }
  if (source->kind == command::SourceKind::kDevice) {
    const auto* source_device =
        source->device_vendor_id ? devices_.Find(*source->device_vendor_id) : nullptr;
    if (source_device == nullptr ||
        source_device->status != device::Status::kOnline) {
      Reject(*source_id, std::nullopt,
             Error("source_device_offline", "来源设备当前离线"), now,
             command_type);
      return;
    }
  }
  const auto parsed = command_type == command::CommandType::kControl
      ? command::ParseSourceControlRequest(message.payload, source->kind)
      : command::ParseSourceConfigRequest(message.payload, source->kind);
  const auto* config = std::get_if<command::SourceConfigRequest>(&parsed);
  const auto* control = std::get_if<command::SourceControlRequest>(&parsed);
  const auto* rejected = std::get_if<command::RejectedSourceRequest>(&parsed);
  const auto request_id = config ? std::optional<std::string>{config->request_id}
      : control ? std::optional<std::string>{control->request_id}
                : rejected->request_id;
  if (!request_id) {
    Reject(*source_id, std::nullopt, rejected->error, now, command_type);
    return;
  }
  if (!mqtt_available_) {
    Reject(*source_id, *request_id,
           Error("mqtt_unavailable", "MQTT当前不可用"), now, command_type);
    return;
  }
  if (!database_available_) {
    Reject(*source_id, *request_id,
           Error("database_unavailable", "数据库当前不可用"), now, command_type);
    return;
  }
  if (active_commands_.size() + operations_.size() + publications_.size() >=
      max_inflight_commands_) {
    Reject(*source_id, *request_id, Error("server_busy", "服务器命令容量已满"),
           now, command_type);
    return;
  }
  RequestContext context{.source_id = *source_id,
                         .source = source,
                         .parsed = parsed,
                         .command_type = command_type,
                         .received_at = message.received_at,
                         .deadline = message.received_at +
                             (command_type == command::CommandType::kControl
                                  ? control_timeout_ : config_timeout_),
                         .target = std::nullopt,
                         .record = std::nullopt,
                         .device_ack_business_status = std::nullopt};
  if (!Submit(OperationKind::kFind, std::move(context),
              FindCommandTask{*source_id, *request_id})) {
    Reject(*source_id, *request_id,
           Error("database_unavailable", "数据库当前不可用"), now, command_type);
  }
}

void CommandService::Handle(CommandDatabaseResult result,
                            command::TimePoint now) {
  const auto found = operations_.find(result.operation_id);
  if (found == operations_.end()) return;
  auto operation = std::move(found->second);
  operations_.erase(found);
  outstanding_operations_.fetch_sub(1, std::memory_order_acq_rel);
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
    const auto comparison = ComparisonPayload(operation.context.parsed);
    if (*existing_value) {
      const auto& existing = **existing_value;
      if (!comparison || command::CompareRequest(existing, *comparison) ==
                             command::IdempotencyResult::kConflict) {
        const auto request_id = RequestId(operation.context.parsed);
        Reject(operation.context.source_id, *request_id,
               Error("idempotency_conflict", "同一请求号的内容不一致"), now,
               operation.context.command_type);
        return;
      }
      operation.context.record = existing;
      operation.context.target = TargetFor(existing);
      PublishAck(operation.context, now);
      return;
    }

    command::CommandRecord record{
        .command_id = command::GenerateUuidV4(),
        .command_type = operation.context.command_type,
        .source_id = operation.context.source_id,
        .request_id = *RequestId(operation.context.parsed),
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
      std::expected<command::ResolvedTarget, command::ProtocolError> target =
          std::unexpected(Error("invalid_request", "命令请求类型非法"));
      if (const auto* request = std::get_if<command::SourceConfigRequest>(
              &operation.context.parsed)) {
        target = command::ResolveConfigTarget(*operation.context.source,
                                              *request, devices_);
      } else if (const auto* request =
                     std::get_if<command::SourceControlRequest>(
                         &operation.context.parsed)) {
        target = command::ResolveControlTarget(*operation.context.source,
                                               *request, devices_);
      }
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
             Error("idempotency_conflict", "同一请求号的内容不一致"), now,
             operation.context.command_type);
      return;
    }
    operation.context.record = *record;
    operation.context.target = TargetFor(*record);
    PublishAck(operation.context, now);
    return;
  }
  if (operation.kind == OperationKind::kTransition &&
      operation.context.record &&
      command::IsTerminal(operation.context.record->status) &&
      !command::IsTerminal(record->status) &&
      command::CanTransition(record->status,
                             operation.context.record->status)) {
    const auto desired = *operation.context.record;
    command::CommandUpdate update{
        .error_code = desired.error_code,
        .error_message = desired.error_message,
        .device_ack = desired.device_ack,
        .dispatched_at = std::nullopt,
        .completed_at = desired.completed_at,
        .updated_at = desired.updated_at};
    SubmitTransitionOrDefer(
        std::move(operation.context),
        TransitionCommandTask{desired.command_id, record->status,
                              desired.status, std::move(update)});
    return;
  }
  const auto transition_requested_status = operation.context.record
      ? std::optional{operation.context.record->status}
      : std::nullopt;
  operation.context.record = *record;
  if (operation.kind == OperationKind::kTransition ||
      command::IsTerminal(record->status)) {
    const bool dispatched_persisted =
        operation.kind == OperationKind::kTransition &&
        transition_requested_status ==
            command::CommandStatus::kDispatched &&
        record->status == command::CommandStatus::kDispatched;
    const auto device_ack_business_status =
        operation.context.device_ack_business_status;
    const bool device_ack_persisted = device_ack_business_status &&
        ((record->status == command::CommandStatus::kInProgress &&
          *device_ack_business_status == "in_progress") ||
         (record->status == command::CommandStatus::kFailed &&
          *device_ack_business_status == "rejected") ||
         (record->status == command::CommandStatus::kTimeout &&
          *device_ack_business_status == "timeout") ||
         (record->status == command::CommandStatus::kSucceeded &&
          *device_ack_business_status != "in_progress" &&
          *device_ack_business_status != "rejected" &&
          *device_ack_business_status != "timeout"));
    PublishAck(operation.context, now);
    if (dispatched_persisted && operation.context.target) {
      Inform("收到来自 " + operation.context.source_id + " 的" +
             DescribeCommand(operation.context.parsed) + "，已路由至设备 " +
             DescribeTarget(*operation.context.target));
    }
    if (device_ack_persisted && operation.context.target) {
      Inform("收到来自设备 " + DescribeTarget(*operation.context.target) +
             "的应答 " + *device_ack_business_status + "，已转发至 " +
             operation.context.source_id);
      operation.context.device_ack_business_status.reset();
    }
    if (command::IsTerminal(record->status)) {
      active_commands_.erase(record->command_id);
      recovery_started_.erase(record->command_id);
      early_device_acks_.erase(record->command_id);
    } else {
      active_commands_.insert_or_assign(record->command_id, operation.context);
      if (record->status == command::CommandStatus::kDispatched) {
        const auto early = early_device_acks_.find(record->command_id);
        if (early != early_device_acks_.end()) {
          auto pending_ack = std::move(early->second);
          early_device_acks_.erase(early);
          HandleDeviceAck(pending_ack.vendor_id, pending_ack.payload,
                          operation.context.command_type,
                          pending_ack.received_at);
        }
      }
    }
    return;
  }
  active_commands_.insert_or_assign(record->command_id, operation.context);
  PublishRecord(std::move(operation.context), now);
}

void CommandService::PublishRecord(RequestContext context,
                                   command::TimePoint now) {
  if (!context.record || !context.record->target_vendor_id || !mqtt_available_) return;
  nlohmann::json payload;
  if (const auto* request =
          std::get_if<command::SourceConfigRequest>(&context.parsed)) {
    payload = command::BuildDeviceConfigSet(context.record->command_id,
                                            request->parameters);
  } else if (const auto* request =
                 std::get_if<command::SourceControlRequest>(&context.parsed)) {
    payload = command::BuildDeviceControlSet(context.record->command_id,
                                             request->command,
                                             request->parameters);
  } else {
    return;
  }
  const auto previous_status = context.record->status;
  recovery_started_.insert(context.record->command_id);
  const auto token = next_publish_token_++;
  publications_.emplace(token, PublishedCommand{context});
  const auto published = device_publisher_(
      token, DeviceSetTopic(topic_namespace_, *context.record->target_vendor_id,
                            context.command_type), payload.dump());
  if (!published) {
    publications_.erase(token);
    auto failed = *context.record;
    failed.status = command::CommandStatus::kFailed;
    failed.error_code = "mqtt_publish_failed";
    failed.error_message = context.command_type == command::CommandType::kControl
                               ? "设备飞控命令发布失败"
                               : "设备配置命令发布失败";
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
    SubmitTransitionOrDefer(
        std::move(context),
        TransitionCommandTask{failed.command_id, previous_status,
                              command::CommandStatus::kFailed, update});
  }
}

void CommandService::HandleDeviceAck(std::string_view vendor_id,
                                     std::string_view payload,
                                     command::CommandType command_type,
                                     command::TimePoint now) {
  std::string command_id;
  std::string business_status;
  nlohmann::json raw;
  std::optional<command::ControlCommand> control_command;
  if (command_type == command::CommandType::kControl) {
    const auto parsed = command::ParseDeviceControlAck(payload);
    if (!parsed) { Diagnose("设备飞控ACK格式非法"); return; }
    command_id = parsed->command_id;
    business_status = parsed->business_status;
    raw = parsed->raw;
    control_command = parsed->command;
  } else {
    const auto parsed = command::ParseDeviceConfigAck(payload);
    if (!parsed) { Diagnose("设备配置ACK格式非法"); return; }
    command_id = parsed->command_id;
    business_status = parsed->business_status;
    raw = parsed->raw;
  }
  const auto found = active_commands_.find(command_id);
  if (found == active_commands_.end() || !found->second.record ||
      found->second.record->target_vendor_id != vendor_id ||
      found->second.command_type != command_type) {
    Diagnose("设备命令ACK无法关联活动命令");
    return;
  }
  auto context = found->second;
  if (control_command) {
    const auto* request = std::get_if<command::SourceControlRequest>(&context.parsed);
    if (request == nullptr || request->command != *control_command) {
      Diagnose("设备飞控ACK的command与活动命令不匹配");
      return;
    }
  }
  const auto current = context.record->status;
  if (current == command::CommandStatus::kPending) {
    const auto early = early_device_acks_.find(command_id);
    if (early == early_device_acks_.end()) {
      early_device_acks_.emplace(
          command_id,
          EarlyDeviceAck{std::string{vendor_id}, std::string{payload}, now});
    } else if (early->second.payload != payload) {
      const auto old_control = command_type == command::CommandType::kControl
          ? command::ParseDeviceControlAck(early->second.payload) : std::unexpected(
              command::ProtocolError{"not_control", "不是飞控命令"});
      const bool incoming_terminal = business_status != "in_progress";
      const bool old_terminal = old_control &&
          old_control->business_status != "in_progress";
      if (!old_terminal || !incoming_terminal) {
        if (incoming_terminal || !old_terminal) {
          early->second = EarlyDeviceAck{std::string{vendor_id},
                                         std::string{payload}, now};
        }
      } else {
        Diagnose("设备飞控ACK在发布确认前发生冲突");
      }
    }
    return;
  }
  if (current != command::CommandStatus::kDispatched &&
      current != command::CommandStatus::kInProgress) return;
  command::CommandStatus desired = command::CommandStatus::kSucceeded;
  if (business_status == "in_progress") desired = command::CommandStatus::kInProgress;
  if (business_status == "rejected") desired = command::CommandStatus::kFailed;
  if (business_status == "timeout") desired = command::CommandStatus::kTimeout;
  auto record = *context.record;
  record.status = desired;
  record.device_ack = raw;
  record.updated_at = now;
  record.completed_at = command::IsTerminal(desired)
                            ? std::optional<command::TimePoint>{now} : std::nullopt;
  if (desired == command::CommandStatus::kInProgress) {
    context.deadline = now + control_timeout_;
  }
  if (desired == command::CommandStatus::kFailed) {
    record.error_code = "device_rejected";
    record.error_message = command_type == command::CommandType::kControl
                               ? "设备拒绝飞控命令" : "设备拒绝配置命令";
  } else if (desired == command::CommandStatus::kTimeout) {
    record.error_code = "device_timeout";
    record.error_message = "设备报告飞控命令超时";
  }
  context.record = record;
  context.device_ack_business_status = business_status;
  command::CommandUpdate update{record.error_code, record.error_message,
                                record.device_ack, std::nullopt,
                                record.completed_at, now};
  SubmitTransitionOrDefer(
      std::move(context),
      TransitionCommandTask{record.command_id, current, desired, update});
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
          .command_type = command::CommandType::kConfig,
          .received_at = now, .deadline = now,
          .target = std::nullopt, .record = std::nullopt,
          .device_ack_business_status = std::nullopt};
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
    if (now >= context.deadline) {
      if (recovery_started_.insert(id).second) {
        auto timed_out = record;
        timed_out.status = command::CommandStatus::kTimeout;
        timed_out.error_code = context.command_type == command::CommandType::kControl
                                   ? "control_timeout" : "command_timeout";
        timed_out.error_message = context.command_type == command::CommandType::kControl
                                      ? "设备飞控命令超时" : "设备配置命令超时";
        timed_out.updated_at = now;
        timed_out.completed_at = now;
        context.record = timed_out;
        command::CommandUpdate update{timed_out.error_code, timed_out.error_message,
                                      std::nullopt, std::nullopt, now, now};
        SubmitTransitionOrDefer(
            std::move(context),
            TransitionCommandTask{id, record.status,
                                  command::CommandStatus::kTimeout, update});
      }
      continue;
    }
    if (context.command_type == command::CommandType::kControl ||
        !mqtt_available_ || recovery_started_.contains(id) ||
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
      : context.command_type == command::CommandType::kControl
          ? command::CommandStatus::kDeliveryUncertain
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
    record.error_code = context.command_type == command::CommandType::kControl
                            ? "control_delivery_uncertain" : "mqtt_publish_failed";
    record.error_message = context.command_type == command::CommandType::kControl
        ? "飞控命令发布后无法确认是否送达" : "设备配置命令发布失败";
    record.completed_at = now;
    update.error_code = record.error_code;
    update.error_message = record.error_message;
    update.completed_at = now;
  }
  context.record = record;
  SubmitTransitionOrDefer(
      std::move(context),
      TransitionCommandTask{record.command_id, command::CommandStatus::kPending,
                            desired, update});
}

void CommandService::SubmitTransitionOrDefer(RequestContext context,
                                             TransitionCommandTask task) {
  const auto command_id = task.command_id;
  auto fallback_context = context;
  auto fallback_task = task;
  if (Submit(OperationKind::kTransition, std::move(context), std::move(task))) {
    deferred_transitions_.erase(command_id);
    return;
  }
  database_available_.store(false, std::memory_order_release);
  deferred_transitions_.insert_or_assign(
      command_id,
      DeferredTransition{std::move(fallback_context), std::move(fallback_task)});
}

void CommandService::RetryDeferredTransitions() {
  if (!database_available_.load(std::memory_order_acquire) ||
      deferred_transitions_.empty()) return;
  auto node = deferred_transitions_.extract(deferred_transitions_.begin());
  auto deferred = std::move(node.mapped());
  const auto command_id = deferred.task.command_id;
  auto fallback_context = deferred.context;
  auto fallback_task = deferred.task;
  if (!Submit(OperationKind::kTransition, std::move(deferred.context),
              std::move(deferred.task))) {
    database_available_.store(false, std::memory_order_release);
    deferred_transitions_.insert_or_assign(
        command_id,
        DeferredTransition{std::move(fallback_context),
                           std::move(fallback_task)});
  }
}

bool CommandService::Submit(OperationKind kind, RequestContext context,
                            CommandDatabaseTask task) {
  const auto operation_id = next_operation_id_++;
  if (!database_submitter_(operation_id, std::move(task))) return false;
  operations_.emplace(operation_id,
                      Operation{kind, std::move(context)});
  outstanding_operations_.fetch_add(1, std::memory_order_release);
  return true;
}

void CommandService::Reject(std::string_view source_id,
                            std::optional<std::string_view> request_id,
                            command::ProtocolError error,
                            command::TimePoint now,
                            command::CommandType command_type) {
  try {
    source_ack_publisher_(
        SourceAckTopic(topic_namespace_, source_id, command_type),
        command::BuildPrePersistenceRejection(request_id, std::move(error), now,
                                              command_type)
            .dump());
  } catch (...) {
    Diagnose("来源配置ACK发布回调异常");
  }
}

void CommandService::PublishAck(const RequestContext& context,
                                command::TimePoint now) {
  if (!context.record) return;
  try {
    source_ack_publisher_(SourceAckTopic(topic_namespace_, context.source_id,
                                        context.command_type),
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

void CommandService::Inform(std::string message) noexcept {
  try {
    if (information_) information_(std::move(message));
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
