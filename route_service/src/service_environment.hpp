// 本文件声明把真实外部适配器接入进程编排边界的运行环境。
#pragma once

#include "core/runtime/service_runner.hpp"

#include <filesystem>
#include <memory>

namespace cns {

class ServiceEnvironment final : public runtime::ServiceOperations {
 public:
  ServiceEnvironment(std::filesystem::path config_path,
                     std::filesystem::path migrations_path);
  ~ServiceEnvironment() override;

  ServiceEnvironment(const ServiceEnvironment&) = delete;
  ServiceEnvironment& operator=(const ServiceEnvironment&) = delete;

  std::expected<void, std::string> LoadConfig() override;
  void InitializeLogger() override;
  std::expected<void, std::string> DiscoverMigrations() override;
  std::expected<void, std::string> ConnectPostgres() override;
  std::expected<void, std::string> ReadAppliedMigrations() override;
  std::expected<void, std::string> PlanMigrations(bool apply) override;
  std::size_t PendingMigrationCount() const override;
  std::expected<void, std::string> ApplyMigration(std::size_t index) override;
  std::expected<void, std::string> LoadDeviceSnapshot() override;
  std::expected<void, std::string> LoadCommandState() override;
  std::expected<void, std::string> InstallSignalHandlers() override;
  std::expected<void, std::string> StartDeviceRuntime() override;
  void StopAcceptingDeviceMessages() override;
  bool StopDeviceRuntime(std::chrono::milliseconds timeout) override;
  std::expected<void, std::string> CreateMqtt() override;
  std::expected<void, std::string> StartMqtt() override;
  bool MqttHasTerminalFailure() const override;
  bool MqttCallbackStopRequested() const override;
  bool ShutdownRequested() const override;
  void WaitForNextCheck() override;
  void StopMqtt() override;
  void Info(std::string_view message) override;
  void Error(std::string_view message) override;

 private:
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace cns
