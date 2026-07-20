// 本文件实现真实外部适配器到可测试进程编排接口的连接。
#include "service_environment.hpp"

#include "adapters/mqtt/mqtt_client.hpp"
#include "adapters/postgres/postgres_store.hpp"
#include "core/config/app_config.hpp"
#include "core/logging/logger.hpp"
#include "core/migration/migration.hpp"
#include "core/migration/migration_plan.hpp"
#include "core/runtime/shutdown_flag.hpp"

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
};

ServiceEnvironment::ServiceEnvironment(std::filesystem::path config_path,
                                       std::filesystem::path migrations_path)
    : state_(std::make_unique<State>(std::move(config_path),
                                     std::move(migrations_path))) {}

ServiceEnvironment::~ServiceEnvironment() = default;

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

std::expected<void, std::string> ServiceEnvironment::StartMqtt() {
  return state_->mqtt->Start();
}

bool ServiceEnvironment::MqttHasTerminalFailure() const {
  return state_->mqtt->GetRuntimeStatus() == mqtt::RuntimeStatus::kTerminalFailure;
}

bool ServiceEnvironment::ShutdownRequested() const {
  return runtime::ShutdownFlag::Requested();
}

void ServiceEnvironment::WaitForNextCheck() {
  std::this_thread::sleep_for(std::chrono::milliseconds{100});
}

void ServiceEnvironment::StopMqtt() {
  state_->mqtt->Stop();
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
