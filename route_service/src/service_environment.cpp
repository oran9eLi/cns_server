// 本文件实现真实外部适配器到可测试进程编排接口的连接。
#include "service_environment.hpp"

#include "adapters/mqtt/mqtt_client.hpp"
#include "adapters/postgres/postgres_store.hpp"
#include "core/config/app_config.hpp"
#include "core/logging/logger.hpp"
#include "core/migration/migration.hpp"
#include "core/migration/migration_plan.hpp"
#include "core/mqtt_topic/device_topic.hpp"
#include "core/runtime/device_service.hpp"
#include "core/runtime/device_ingress.hpp"
#include "core/runtime/command_service.hpp"
#include "core/runtime/command_ingress.hpp"
#include "core/runtime/postgres_worker.hpp"
#include "core/runtime/shutdown_flag.hpp"
#include "core/state_event/state_event.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <iostream>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace cns {
namespace {

void HandleShutdownSignal(int) {
  runtime::ShutdownFlag::Request();
}

class RuntimePostgresBridge final : public runtime::PostgresWorker::StorePort {
 public:
  RuntimePostgresBridge(std::unique_ptr<postgres::PostgresStore> store,
                        config::DatabaseConfig config,
                        postgres::PostgresStore::InfoSink info_sink,
                        std::vector<migration::Migration> migrations)
      : store_(std::move(store)), config_(std::move(config)),
        info_sink_(std::move(info_sink)), migrations_(std::move(migrations)) {}

  std::expected<device::DeviceRecord, runtime::DatabaseError> Provision(
      const protocol::Registration& registration,
      runtime::TimePoint received_at) override {
    auto result = store_->ProvisionDevice({registration, received_at});
    if (result) return std::move(*result);
    return std::unexpected(Classify("数据库建档失败"));
  }

  std::expected<void, runtime::DatabaseError> Write(
      const persistence::DesiredDeviceWrite& write) override {
    auto result = store_->WriteDeviceState(write);
    if (result) return {};
    return std::unexpected(Classify("数据库状态写入失败"));
  }

  std::expected<std::optional<command::CommandRecord>, runtime::DatabaseError>
  FindCommand(std::string_view source_id, std::string_view request_id) override {
    auto result = store_->FindCommand(source_id, request_id);
    if (result) return std::move(*result);
    return std::unexpected(ClassifyWithDetail("数据库命令查询失败",
                                              result.error()));
  }

  std::expected<std::optional<command::CommandRecord>, runtime::DatabaseError>
  FindCommandById(std::string_view command_id) override {
    auto result = store_->FindCommandById(command_id);
    if (result) return std::move(*result);
    return std::unexpected(ClassifyWithDetail("数据库命令查询失败",
                                              result.error()));
  }

  std::expected<command::CommandRecord, runtime::DatabaseError> InsertCommand(
      const command::CommandRecord& command) override {
    auto result = store_->InsertCommand(command);
    if (result) return std::move(*result);
    return std::unexpected(ClassifyWithDetail("数据库命令写入失败",
                                              result.error()));
  }

  std::expected<command::CommandRecord, runtime::DatabaseError>
  TransitionCommand(std::string_view command_id,
                    command::CommandStatus expected,
                    command::CommandStatus desired,
                    const command::CommandUpdate& update) override {
    auto result = store_->TransitionCommand(command_id, expected, desired, update);
    if (result) return std::move(*result);
    return std::unexpected(ClassifyWithDetail("数据库命令状态转换失败",
                                              result.error()));
  }

  std::expected<std::size_t, runtime::DatabaseError> CleanupCommands(
      command::TimePoint before, std::size_t batch_size) override {
    auto result = store_->DeleteExpiredTerminalCommands(before, batch_size);
    if (result) return *result;
    return std::unexpected(Classify("数据库命令清理失败"));
  }

  std::expected<std::vector<command::CommandRecord>, runtime::DatabaseError>
  RecoverControlCommands(command::TimePoint recovered_at,
                         std::size_t batch_size) override {
    auto result = store_->RecoverActiveControlCommands(recovered_at, batch_size);
    if (result) return std::move(*result);
    return std::unexpected(Classify("数据库飞控命令恢复失败"));
  }

  std::expected<void, runtime::DatabaseError> ReconnectAndValidate() override {
    auto connected = postgres::PostgresStore::Connect(config_, info_sink_);
    if (!connected) {
      return std::unexpected(runtime::DatabaseError{
          runtime::DatabaseError::Kind::kUnavailable, "数据库重连失败"});
    }
    auto applied = (*connected)->ReadAppliedMigrations();
    if (!applied) {
      const auto kind = (*connected)->LastOperationFailureKind() ==
                                postgres::OperationFailureKind::kUnavailable
                            ? runtime::DatabaseError::Kind::kUnavailable
                            : runtime::DatabaseError::Kind::kPermanent;
      return std::unexpected(runtime::DatabaseError{
          kind,
          "数据库重连后读取迁移版本失败"});
    }
    auto plan = migration::BuildMigrationPlan(
        migrations_, *applied, migration::Mode::kCheckOnly);
    if (!plan) {
      return std::unexpected(runtime::DatabaseError{
          runtime::DatabaseError::Kind::kPermanent,
          "数据库迁移版本校验失败"});
    }
    store_ = std::move(*connected);
    return {};
  }

 private:
  runtime::DatabaseError Classify(std::string context) const {
    const auto kind = store_ && store_->LastOperationFailureKind() ==
                                    postgres::OperationFailureKind::kPermanent
                          ? runtime::DatabaseError::Kind::kPermanent
                          : runtime::DatabaseError::Kind::kUnavailable;
    return {kind, std::move(context)};
  }

  runtime::DatabaseError ClassifyWithDetail(std::string context,
                                            const std::string& detail) const {
    auto error = Classify(std::move(context));
    if (!detail.empty()) error.message = detail;
    return error;
  }

  std::unique_ptr<postgres::PostgresStore> store_;
  config::DatabaseConfig config_;
  postgres::PostgresStore::InfoSink info_sink_;
  std::vector<migration::Migration> migrations_;
};

struct RuntimeExternalBridge {
  std::mutex mutex;
  mqtt::MqttClient* mqtt = nullptr;
  logging::Logger* logger = nullptr;
  std::string topic_namespace;
  bool enabled = true;

  void Disable() noexcept {
    std::lock_guard lock(mutex);
    enabled = false;
    mqtt = nullptr;
    logger = nullptr;
  }

  void Diagnose(std::string_view message) noexcept {
    std::lock_guard lock(mutex);
    if (!enabled || logger == nullptr) return;
    try { logger->Error(message); } catch (...) {}
  }

  void Inform(std::string message) noexcept {
    std::lock_guard lock(mutex);
    if (!enabled || logger == nullptr) return;
    try { logger->Info(message); } catch (...) {}
  }

  void Publish(runtime::PublishedState published) noexcept {
    std::lock_guard lock(mutex);
    if (!enabled || mqtt == nullptr || logger == nullptr) return;
    try {
      state_event::Snapshot snapshot{
          .vendor_id = published.record.vendor_id,
          .school_name = published.record.school_name,
          .dcdw_label = published.record.dcdw_label,
          .online = published.record.status == device::Status::kOnline,
          .last_seen_at = published.record.last_seen_at,
          .telemetry_received_at = published.record.telemetry_received_at,
          .latest_telemetry = published.record.latest_telemetry,
          .degraded = published.degraded,
      };
      const auto payload = state_event::BuildStateEvent(
          snapshot, published.reason, std::chrono::system_clock::now()).dump();
      auto result = mqtt->PublishStateEvent(
          mqtt_topic::StateEventTopic(topic_namespace,
                                      published.record.vendor_id),
          payload);
      if (!result) logger->Error("发布设备状态事件失败");
    } catch (...) {
      try { logger->Error("发布设备状态事件发生异常"); } catch (...) {}
    }
  }
};

struct RuntimeBundle final : std::enable_shared_from_this<RuntimeBundle> {
  config::AppConfig config;
  device::DeviceRegistry registry;
  std::vector<persistence::DesiredDeviceWrite> startup_writes;
  command::SourceCatalog command_sources;
  std::vector<command::CommandRecord> active_commands;
  std::unique_ptr<RuntimePostgresBridge> store;
  std::unique_ptr<runtime::PostgresWorker> worker;
  std::unique_ptr<runtime::DeviceService> service;
  std::unique_ptr<runtime::CommandService> command_service;
  std::shared_ptr<runtime::DeviceIngress> ingress;
  std::shared_ptr<runtime::CommandIngress> command_ingress;
  std::weak_ptr<RuntimeExternalBridge> external;
  runtime::SelfOwnedRuntimeThread database_thread;
  runtime::SelfOwnedRuntimeThread business_thread;

  void Initialize() {
    const std::weak_ptr<RuntimeBundle> weak_self = shared_from_this();
    worker = std::make_unique<runtime::PostgresWorker>(
        *store,
        [weak_self](runtime::DatabaseResult result) {
          if (const auto self = weak_self.lock(); self && self->service) {
            self->service->PushDatabaseResult(std::move(result));
          }
        },
        [weak_self] {
          const auto self = weak_self.lock();
          if (!self) return;
          const auto bridge = self->external.lock();
          if (!bridge) return;
          std::lock_guard lock(bridge->mutex);
          if (!bridge->enabled || bridge->mqtt == nullptr) return;
          auto replay = bridge->mqtt->ReplayRetainedRegistrations(
              self->config.mqtt.topic_namespace);
          if (!replay && bridge->logger) {
            bridge->logger->Error("重放设备注册消息失败");
          }
        },
        [] { return std::chrono::steady_clock::now(); },
        [weak = external](std::string message) {
          if (const auto bridge = weak.lock()) bridge->Diagnose(message);
        },
        config.database.reconnect_interval,
        [weak_self](runtime::CommandDatabaseResult result) {
          if (const auto self = weak_self.lock(); self && self->command_service) {
            self->command_service->PushDatabaseResult(std::move(result));
          }
        },
        config.command.max_inflight_commands);
    service = std::make_unique<runtime::DeviceService>(
        registry,
        [weak_self](protocol::Registration registration,
                    runtime::TimePoint at) {
          const auto self = weak_self.lock();
          return self && self->worker &&
                 self->worker->SubmitProvision(std::move(registration), at);
        },
        [weak_self](persistence::DesiredDeviceWrite write) {
          const auto self = weak_self.lock();
          return self && self->worker &&
                 self->worker->SubmitWrite(std::move(write));
        },
        [weak_self, weak = external](runtime::PublishedState published) {
          if (published.record.status == device::Status::kOnline) {
            if (const auto self = weak_self.lock(); self && self->command_service) {
              self->command_service->OnTargetOnline(
                  published.record.vendor_id, std::chrono::system_clock::now());
            }
          }
          if (const auto bridge = weak.lock()) bridge->Publish(std::move(published));
        },
        [] { return std::chrono::steady_clock::now(); },
        [weak = external](std::string message) {
          if (const auto bridge = weak.lock()) bridge->Diagnose(message);
        },
        config.queues.mqtt_inbound_capacity, config.mqtt.topic_namespace,
        config.device_state.telemetry_flush_interval,
        config.device_state.offline_timeout,
        [weak = external](std::string message) {
          if (const auto bridge = weak.lock()) bridge->Inform(message);
        });
    ingress = std::make_shared<runtime::DeviceIngress>(
        [weak_self](mqtt::InboundMessage message) {
          const auto self = weak_self.lock();
          return self && self->service &&
                 self->service->TryPush(std::move(message));
        },
        [weak = external](std::string message) {
          if (const auto bridge = weak.lock()) bridge->Diagnose(message);
        });
    command_service = std::make_unique<runtime::CommandService>(
        command_sources, registry,
        [weak_self](std::uint64_t id, runtime::CommandDatabaseTask task) {
          const auto self = weak_self.lock();
          return self && self->worker &&
                 self->worker->SubmitCommand(id, std::move(task));
        },
        [weak = external](std::uint64_t token, std::string topic,
                          std::string payload) -> std::expected<void, std::string> {
          const auto bridge = weak.lock();
          if (!bridge) return std::unexpected("MQTT桥已失效");
          std::lock_guard lock(bridge->mutex);
          if (!bridge->enabled || bridge->mqtt == nullptr)
            return std::unexpected("MQTT桥已失效");
          return bridge->mqtt->PublishCommandSet(token, topic, payload);
        },
        [weak = external](std::string topic, std::string payload) {
          const auto bridge = weak.lock();
          if (!bridge) return;
          std::lock_guard lock(bridge->mutex);
          if (bridge->enabled && bridge->mqtt != nullptr) {
            static_cast<void>(bridge->mqtt->PublishSourceCommandAck(topic, payload));
          }
        },
        [weak = external](std::string message) {
          if (const auto bridge = weak.lock()) bridge->Diagnose(message);
        }, config.command.max_inflight_commands, config.mqtt.topic_namespace,
        config.command.config_timeout, config.command.control_timeout,
        config.command.terminal_retention,
        config.command.cleanup_interval, config.command.cleanup_batch_size,
        [weak = external](std::string message) {
          if (const auto bridge = weak.lock()) bridge->Inform(std::move(message));
        });
    command_service->LoadActive(std::move(active_commands));
    command_service->SetMqttAvailable(false);
    command_ingress = std::make_shared<runtime::CommandIngress>(
        *command_service, config.mqtt.topic_namespace,
        [weak = external](std::string message) {
          if (const auto bridge = weak.lock()) bridge->Diagnose(message);
        });
  }

  std::expected<void, std::string> StartDatabaseThread() {
    const auto self = shared_from_this();
    database_thread.Start(self, [this](std::stop_token stop) {
      worker->Run(stop);
    });
    return {};
  }

  std::expected<void, std::string> StartBusinessThread() {
    const auto self = shared_from_this();
    business_thread.Start(self, [this](std::stop_token stop) {
      service->Run(stop, [this](runtime::TimePoint now) {
        command_service->ProcessReady(now);
      });
    });
    for (auto& write : startup_writes) {
      static_cast<void>(worker->SubmitWrite(std::move(write)));
    }
    startup_writes.clear();
    return {};
  }

  void RequestStop() noexcept {
    business_thread.RequestStop();
    database_thread.RequestStop();
    if (service) service->CancelOutstandingDatabaseWork();
    if (command_service) command_service->CancelOutstandingWork();
  }

  void Join() {
    database_thread.Join();
    business_thread.Join();
  }

  void Detach() noexcept {
    database_thread.Detach();
    business_thread.Detach();
  }
};

}  // namespace

struct ServiceEnvironment::State {
  State(std::filesystem::path config_path_value,
        std::filesystem::path migrations_path_value)
      : config_path(std::move(config_path_value)),
        migrations_path(std::move(migrations_path_value)) {}

  std::filesystem::path config_path;
  std::filesystem::path migrations_path;
  std::optional<config::AppConfig> config;
  std::shared_ptr<logging::Logger> logger;
  std::vector<migration::Migration> available;
  std::unique_ptr<postgres::PostgresStore> store;
  std::vector<migration::AppliedMigration> applied;
  std::optional<migration::MigrationPlan> plan;
  std::unique_ptr<mqtt::MqttClient> mqtt;
  device::DeviceRegistry registry;
  std::vector<persistence::DesiredDeviceWrite> startup_writes;
  command::SourceCatalog command_sources;
  std::vector<command::CommandRecord> active_commands;
  std::shared_ptr<RuntimeExternalBridge> external_bridge;
  std::shared_ptr<RuntimeBundle> runtime_bundle;
  std::chrono::steady_clock::time_point next_mqtt_subscription_retry{};
  bool accepting_device_messages = false;
  bool device_runtime_stopped = true;
};

ServiceEnvironment::ServiceEnvironment(std::filesystem::path config_path,
                                       std::filesystem::path migrations_path)
    : state_(std::make_unique<State>(std::move(config_path),
                                     std::move(migrations_path))) {}

ServiceEnvironment::~ServiceEnvironment() {
  StopAcceptingDeviceMessages();
  static_cast<void>(StopDeviceRuntime(std::chrono::milliseconds{5000}));
  StopMqtt();
}

std::expected<void, std::string> ServiceEnvironment::LoadConfig() {
  auto loaded = config::LoadAppConfig(state_->config_path);
  if (!loaded) return std::unexpected(loaded.error());
  state_->config = std::move(*loaded);
  return {};
}

void ServiceEnvironment::InitializeLogger() {
  state_->logger = std::make_shared<logging::Logger>(
      state_->config->logging.level, std::cout, std::cerr);
}

std::expected<void, std::string> ServiceEnvironment::DiscoverMigrations() {
  auto discovered = migration::DiscoverMigrations(state_->migrations_path);
  if (!discovered) return std::unexpected(discovered.error());
  state_->available = std::move(*discovered);
  return {};
}

std::expected<void, std::string> ServiceEnvironment::ConnectPostgres() {
  auto connected = postgres::PostgresStore::Connect(state_->config->database,
                                                     *state_->logger);
  if (!connected) return std::unexpected(connected.error());
  state_->store = std::move(*connected);
  return {};
}

std::expected<void, std::string> ServiceEnvironment::ReadAppliedMigrations() {
  auto read = state_->store->ReadAppliedMigrations();
  if (!read) return std::unexpected(read.error());
  state_->applied = std::move(*read);
  return {};
}

std::expected<void, std::string> ServiceEnvironment::PlanMigrations(bool apply) {
  auto planned = migration::BuildMigrationPlan(
      state_->available, state_->applied,
      apply ? migration::Mode::kApply : migration::Mode::kCheckOnly);
  if (!planned) return std::unexpected(planned.error());
  state_->plan = std::move(*planned);
  return {};
}

std::size_t ServiceEnvironment::PendingMigrationCount() const {
  return state_->plan->pending.size();
}

std::expected<void, std::string> ServiceEnvironment::ApplyMigration(
    std::size_t index) {
  return state_->store->ApplyMigration(state_->plan->pending.at(index));
}

std::expected<void, std::string> ServiceEnvironment::LoadDeviceSnapshot() {
  auto records = state_->store->LoadDevices();
  if (!records) return std::unexpected(records.error());
  auto loaded = state_->registry.Load(std::move(*records));
  if (!loaded) return std::unexpected(loaded.error());
  state_->startup_writes.clear();
  for (auto& mutation : state_->registry.ExpireInactive(
           std::chrono::system_clock::now(),
           state_->config->device_state.offline_timeout)) {
    const auto revision = mutation.record.revision;
    state_->startup_writes.push_back({
        .record = std::move(mutation.record),
        .revision = revision,
        .write_metadata = false,
        .write_status = true,
        .write_telemetry = false,
        .urgency = persistence::Urgency::kImmediate,
    });
  }
  return {};
}

std::expected<void, std::string> ServiceEnvironment::LoadCommandState() {
  auto sources = state_->store->SyncAndLoadCommandSources(
      state_->config->command.fixed_sources);
  if (!sources) return std::unexpected(sources.error());
  if (auto loaded = state_->command_sources.Load(std::move(*sources)); !loaded) {
    return std::unexpected(loaded.error());
  }
  auto commands = state_->store->LoadActiveCommands(
      state_->config->command.max_inflight_commands + 1);
  if (!commands) return std::unexpected(commands.error());
  if (commands->size() > state_->config->command.max_inflight_commands) {
    return std::unexpected("活动命令数量超过配置上限");
  }
  auto recovered = state_->store->RecoverActiveControlCommands(
      std::chrono::system_clock::now(),
      state_->config->command.max_inflight_commands + 1);
  if (!recovered) return std::unexpected(recovered.error());
  state_->active_commands.clear();
  for (auto& record : *commands) {
    if (record.command_type == command::CommandType::kConfig) {
      state_->active_commands.push_back(std::move(record));
    }
  }
  return {};
}

std::expected<void, std::string> ServiceEnvironment::InstallSignalHandlers() {
  runtime::ShutdownFlag::ResetForTesting();
  if (std::signal(SIGINT, HandleShutdownSignal) == SIG_ERR ||
      std::signal(SIGTERM, HandleShutdownSignal) == SIG_ERR) {
    return std::unexpected("安装退出信号处理器失败");
  }
  return {};
}

std::expected<void, std::string> ServiceEnvironment::CreateMqtt() {
  auto created = mqtt::MqttClient::Create(state_->config->mqtt, *state_->logger);
  if (!created) return std::unexpected(created.error());
  state_->mqtt = std::move(*created);
  return {};
}

std::expected<void, std::string> ServiceEnvironment::StartDeviceRuntime() {
  try {
    state_->external_bridge = std::make_shared<RuntimeExternalBridge>();
    state_->external_bridge->logger = state_->logger.get();
    state_->external_bridge->topic_namespace =
        state_->config->mqtt.topic_namespace;

    auto bundle = std::make_shared<RuntimeBundle>();
    bundle->config = *state_->config;
    bundle->registry = std::move(state_->registry);
    bundle->startup_writes = std::move(state_->startup_writes);
    bundle->command_sources = std::move(state_->command_sources);
    bundle->active_commands = std::move(state_->active_commands);
    bundle->external = state_->external_bridge;
    const postgres::PostgresStore::InfoSink postgres_info =
        [weak = std::weak_ptr<RuntimeExternalBridge>{state_->external_bridge}](
            std::string message) {
          if (const auto bridge = weak.lock()) bridge->Inform(std::move(message));
        };
    state_->store->SetInfoSink(postgres_info);
    bundle->store = std::make_unique<RuntimePostgresBridge>(
        std::move(state_->store), state_->config->database, postgres_info,
        state_->available);
    bundle->Initialize();
    state_->runtime_bundle = bundle;
    state_->device_runtime_stopped = false;
    state_->accepting_device_messages = true;

    runtime::DeviceRuntimeStartOperations start_operations{
        .configure_handler = []() -> std::expected<void, std::string> { return {}; },
        .start_postgres_thread = [bundle]() {
          return bundle->StartDatabaseThread();
        },
        .start_device_thread = [bundle]() {
          return bundle->StartBusinessThread();
        },
        .rollback = [this, bundle] {
          bundle->RequestStop();
          bundle->Join();
          if (state_->external_bridge) state_->external_bridge->Disable();
          state_->runtime_bundle.reset();
          state_->device_runtime_stopped = true;
          state_->accepting_device_messages = false;
        },
    };
    return runtime::StartDeviceRuntimeTransaction(start_operations);
  } catch (const std::exception&) {
    if (state_->runtime_bundle) {
      state_->runtime_bundle->RequestStop();
      state_->runtime_bundle->Join();
      state_->runtime_bundle.reset();
    }
    if (state_->external_bridge) state_->external_bridge->Disable();
    state_->device_runtime_stopped = true;
    state_->accepting_device_messages = false;
    return std::unexpected("启动设备运行时失败");
  } catch (...) {
    if (state_->runtime_bundle) {
      state_->runtime_bundle->RequestStop();
      state_->runtime_bundle->Join();
      state_->runtime_bundle.reset();
    }
    if (state_->external_bridge) state_->external_bridge->Disable();
    state_->device_runtime_stopped = true;
    state_->accepting_device_messages = false;
    return std::unexpected("启动设备运行时发生未知异常");
  }
}

void ServiceEnvironment::StopAcceptingDeviceMessages() {
  if (!state_ || !state_->accepting_device_messages) return;
  state_->accepting_device_messages = false;
  if (state_->runtime_bundle && state_->runtime_bundle->ingress) {
    state_->runtime_bundle->ingress->Disable();
  }
  if (state_->runtime_bundle && state_->runtime_bundle->command_ingress) {
    state_->runtime_bundle->command_ingress->Disable();
  }
  if (state_->mqtt) {
    auto result = state_->mqtt->ConfigureBusinessMessages(
        {}, state_->config->mqtt.max_payload_bytes);
    if (!result && state_->logger) state_->logger->Error("停止接收设备消息失败");
  }
  if (state_->runtime_bundle && state_->runtime_bundle->service) {
    state_->runtime_bundle->service->Close();
  }
  if (state_->runtime_bundle && state_->runtime_bundle->command_service) {
    state_->runtime_bundle->command_service->Close();
  }
}

bool ServiceEnvironment::StopDeviceRuntime(std::chrono::milliseconds timeout) {
  if (!state_ || state_->device_runtime_stopped) return true;
  state_->device_runtime_stopped = true;
  const auto bundle = state_->runtime_bundle;
  if (!bundle) return true;
  runtime::DeviceRuntimeDrainOperations operations{
      .wait_input_drained = [bundle](std::chrono::milliseconds wait) {
        return bundle->service->WaitForInputDrained(wait);
      },
      .wait_database_idle = [bundle](std::chrono::milliseconds wait) {
        const auto deadline = std::chrono::steady_clock::now() + wait;
        if (!bundle->service->WaitForDatabaseIdle(wait)) return false;
        const auto remaining = std::max(
            std::chrono::milliseconds{0},
            std::chrono::ceil<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()));
        return bundle->command_service->WaitForDatabaseIdle(remaining);
      },
      .flush_database = [bundle](std::chrono::milliseconds wait) {
        return bundle->worker->FlushAndStop(wait);
      },
      .pending_devices = [bundle] {
        return bundle->worker->PendingDeviceCount();
      },
      .disable_external_bridge = [bridge = state_->external_bridge] {
        if (bridge) bridge->Disable();
      },
      .request_stop = [bundle] { bundle->RequestStop(); },
      .join_threads = [bundle] { bundle->Join(); },
      .detach_threads = [bundle] { bundle->Detach(); },
      .report_timeout = [logger = state_->logger](std::size_t pending) {
        logger->Error("数据库排空超时，未持久化设备数量：" +
                      std::to_string(pending));
      },
  };
  const bool drained = runtime::DrainDeviceRuntime(timeout, operations);
  state_->runtime_bundle.reset();
  return drained;
}

std::expected<void, std::string> ServiceEnvironment::StartMqtt() {
  const auto bundle = state_->runtime_bundle;
  if (!bundle) return std::unexpected("设备运行时尚未启动");
  {
    std::lock_guard lock(state_->external_bridge->mutex);
    state_->external_bridge->mqtt = state_->mqtt.get();
  }
  auto configured = state_->mqtt->ConfigureBusinessMessages(
      [weak = std::weak_ptr<runtime::DeviceIngress>{bundle->ingress},
       command_weak = std::weak_ptr<runtime::CommandIngress>{bundle->command_ingress}]
      (mqtt::InboundMessage message) {
        if (const auto command_ingress = command_weak.lock())
          command_ingress->TryPush(message);
        if (const auto ingress = weak.lock()) ingress->Handle(std::move(message));
      }, state_->config->mqtt.max_payload_bytes);
  if (!configured) return std::unexpected(configured.error());
  auto publishing = state_->mqtt->ConfigureCommandPublishing(
      [weak = std::weak_ptr<RuntimeBundle>{bundle}](mqtt::PublishCompletion completion) {
        if (const auto runtime = weak.lock(); runtime && runtime->command_service) {
          runtime->command_service->PushPublishCompletion(std::move(completion));
        }
      }, state_->config->command.max_inflight_commands);
  if (!publishing) return std::unexpected(publishing.error());
  auto devices = state_->mqtt->SubscribeDeviceMessages(
      state_->config->mqtt.topic_namespace);
  if (!devices) return std::unexpected(devices.error());
  auto commands = state_->mqtt->SubscribeCommandMessages(
      state_->config->mqtt.topic_namespace);
  if (!commands) return std::unexpected(commands.error());
  return state_->mqtt->Start();
}

bool ServiceEnvironment::MqttHasTerminalFailure() const {
  return state_->mqtt->GetRuntimeStatus() == mqtt::RuntimeStatus::kTerminalFailure;
}

bool ServiceEnvironment::MqttCallbackStopRequested() const {
  return state_->mqtt->CallbackStopRequested();
}

bool ServiceEnvironment::ShutdownRequested() const {
  return runtime::ShutdownFlag::Requested();
}

void ServiceEnvironment::WaitForNextCheck() {
  if (state_->runtime_bundle && state_->runtime_bundle->command_service &&
      state_->runtime_bundle->worker && state_->mqtt) {
    state_->runtime_bundle->command_service->SetDatabaseAvailable(
        state_->runtime_bundle->worker->IsDatabaseAvailable());
    const bool mqtt_connected = state_->mqtt->IsConnected();
    state_->runtime_bundle->command_service->SetMqttAvailable(mqtt_connected);
    const auto now = std::chrono::steady_clock::now();
    if (mqtt_connected && now >= state_->next_mqtt_subscription_retry) {
      state_->next_mqtt_subscription_retry = now + std::chrono::seconds{5};
      auto subscribed = state_->mqtt->EnsureBusinessSubscriptions();
      if (!subscribed && state_->logger) {
        state_->logger->Warn("MQTT业务订阅重试失败：" + subscribed.error());
      }
    }
  }
  std::this_thread::sleep_for(std::chrono::milliseconds{100});
}

void ServiceEnvironment::StopMqtt() {
  if (state_ && state_->mqtt) state_->mqtt->Stop();
}

void ServiceEnvironment::Info(std::string_view message) {
  state_->logger->Info(message);
}

void ServiceEnvironment::Error(std::string_view message) {
  if (state_->logger) {
    state_->logger->Error(message);
  } else {
    std::cerr << "错误：" << message << '\n';
  }
}

}  // namespace cns
