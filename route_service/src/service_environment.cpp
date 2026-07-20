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
#include "core/runtime/postgres_worker.hpp"
#include "core/runtime/shutdown_flag.hpp"
#include "core/state_event/state_event.hpp"

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
  RuntimePostgresBridge(std::unique_ptr<postgres::PostgresStore>& store,
                        const config::DatabaseConfig& config,
                        logging::Logger& logger,
                        const std::vector<migration::Migration>& migrations)
      : store_(store), config_(config), logger_(logger), migrations_(migrations) {}

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

  std::expected<void, runtime::DatabaseError> ReconnectAndValidate() override {
    auto connected = postgres::PostgresStore::Connect(config_, logger_);
    if (!connected) {
      return std::unexpected(runtime::DatabaseError{
          runtime::DatabaseError::Kind::kUnavailable, "数据库重连失败"});
    }
    auto applied = (*connected)->ReadAppliedMigrations();
    if (!applied) {
      return std::unexpected(runtime::DatabaseError{
          runtime::DatabaseError::Kind::kUnavailable,
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

  std::unique_ptr<postgres::PostgresStore>& store_;
  const config::DatabaseConfig& config_;
  logging::Logger& logger_;
  const std::vector<migration::Migration>& migrations_;
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
  std::unique_ptr<logging::Logger> logger;
  std::vector<migration::Migration> available;
  std::unique_ptr<postgres::PostgresStore> store;
  std::vector<migration::AppliedMigration> applied;
  std::optional<migration::MigrationPlan> plan;
  std::unique_ptr<mqtt::MqttClient> mqtt;
  device::DeviceRegistry registry;
  std::vector<persistence::DesiredDeviceWrite> startup_writes;
  std::unique_ptr<RuntimePostgresBridge> runtime_store;
  std::unique_ptr<runtime::PostgresWorker> postgres_worker;
  std::unique_ptr<runtime::DeviceService> device_service;
  std::jthread postgres_thread;
  std::jthread device_thread;
  bool accepting_device_messages = false;
  bool device_runtime_stopped = true;
};

ServiceEnvironment::ServiceEnvironment(std::filesystem::path config_path,
                                       std::filesystem::path migrations_path)
    : state_(std::make_unique<State>(std::move(config_path),
                                     std::move(migrations_path))) {}

ServiceEnvironment::~ServiceEnvironment() {
  StopAcceptingDeviceMessages();
  StopDeviceRuntime();
  StopMqtt();
}

std::expected<void, std::string> ServiceEnvironment::LoadConfig() {
  auto loaded = config::LoadAppConfig(state_->config_path);
  if (!loaded) return std::unexpected(loaded.error());
  state_->config = std::move(*loaded);
  return {};
}

void ServiceEnvironment::InitializeLogger() {
  state_->logger = std::make_unique<logging::Logger>(
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
  state_->runtime_store = std::make_unique<RuntimePostgresBridge>(
      state_->store, state_->config->database, *state_->logger,
      state_->available);
  state_->postgres_worker = std::make_unique<runtime::PostgresWorker>(
      *state_->runtime_store,
      [this](runtime::DatabaseResult result) {
        if (state_->device_service) {
          state_->device_service->PushDatabaseResult(std::move(result));
        }
      },
      [this] {
        auto replay = state_->mqtt->ReplayRetainedRegistrations(
            state_->config->mqtt.topic_namespace);
        if (!replay) state_->logger->Error("重放设备注册消息失败");
      },
      [] { return std::chrono::steady_clock::now(); },
      [this](std::string message) { state_->logger->Error(message); },
      state_->config->database.reconnect_interval);
  state_->device_service = std::make_unique<runtime::DeviceService>(
      state_->registry,
      [this](protocol::Registration registration, runtime::TimePoint at) {
        return state_->postgres_worker->SubmitProvision(std::move(registration),
                                                        at);
      },
      [this](persistence::DesiredDeviceWrite write) {
        return state_->postgres_worker->SubmitWrite(std::move(write));
      },
      [this](runtime::PublishedState published) {
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
        auto result = state_->mqtt->PublishStateEvent(
            mqtt_topic::StateEventTopic(state_->config->mqtt.topic_namespace,
                                        published.record.vendor_id),
            payload);
        if (!result) state_->logger->Error("发布设备状态事件失败");
      },
      [] { return std::chrono::steady_clock::now(); },
      [this](std::string message) { state_->logger->Error(message); },
      state_->config->queues.mqtt_inbound_capacity,
      state_->config->mqtt.topic_namespace,
      state_->config->device_state.telemetry_flush_interval,
      state_->config->device_state.offline_timeout);
  auto configured = state_->mqtt->ConfigureBusinessMessages(
      [this](mqtt::InboundMessage message) {
        if (state_->device_service) {
          static_cast<void>(state_->device_service->TryPush(std::move(message)));
        }
      },
      state_->config->mqtt.max_payload_bytes);
  if (!configured) return std::unexpected(configured.error());
  auto subscribed = state_->mqtt->SubscribeDeviceMessages(
      state_->config->mqtt.topic_namespace);
  if (!subscribed) return std::unexpected(subscribed.error());

  state_->device_runtime_stopped = false;
  state_->accepting_device_messages = true;
  state_->postgres_thread = std::jthread(
      [this](std::stop_token stop) { state_->postgres_worker->Run(stop); });
  state_->device_thread = std::jthread(
      [this](std::stop_token stop) { state_->device_service->Run(stop); });
  for (auto& write : state_->startup_writes) {
    if (!state_->postgres_worker->SubmitWrite(std::move(write))) {
      state_->logger->Error("启动阶段离线状态提交失败");
    }
  }
  state_->startup_writes.clear();
  return {};
}

void ServiceEnvironment::StopAcceptingDeviceMessages() {
  if (!state_ || !state_->accepting_device_messages) return;
  state_->accepting_device_messages = false;
  if (state_->mqtt) {
    auto result = state_->mqtt->ConfigureBusinessMessages(
        {}, state_->config->mqtt.max_payload_bytes);
    if (!result && state_->logger) state_->logger->Error("停止接收设备消息失败");
  }
  if (state_->device_service) state_->device_service->Close();
}

void ServiceEnvironment::StopDeviceRuntime() {
  if (!state_ || state_->device_runtime_stopped) return;
  state_->device_runtime_stopped = true;
  if (state_->device_thread.joinable()) state_->device_thread.join();
  if (state_->postgres_worker) {
    const bool flushed = state_->postgres_worker->FlushAndStop(
        std::chrono::seconds{5});
    if (!flushed) {
      state_->logger->Error(
          "数据库排空超时，未持久化设备数量：" +
          std::to_string(state_->postgres_worker->PendingDeviceCount()));
      state_->postgres_thread.request_stop();
    }
  }
  if (state_->postgres_thread.joinable()) state_->postgres_thread.join();
}

std::expected<void, std::string> ServiceEnvironment::StartMqtt() {
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
