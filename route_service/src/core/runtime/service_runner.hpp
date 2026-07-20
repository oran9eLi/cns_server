// 本文件声明不依赖外部服务实现的进程生命周期编排边界。
#pragma once

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

namespace cns::runtime {

enum class RunMode { kNormal, kMigrateOnly };

/** 提供进程编排所需的最小外部操作集合。 */
class ServiceOperations {
 public:
  virtual ~ServiceOperations() = default;
  virtual std::expected<void, std::string> LoadConfig() = 0;
  virtual void InitializeLogger() = 0;
  virtual std::expected<void, std::string> DiscoverMigrations() = 0;
  virtual std::expected<void, std::string> ConnectPostgres() = 0;
  virtual std::expected<void, std::string> ReadAppliedMigrations() = 0;
  virtual std::expected<void, std::string> PlanMigrations(bool apply) = 0;
  virtual std::size_t PendingMigrationCount() const = 0;
  virtual std::expected<void, std::string> ApplyMigration(std::size_t index) = 0;
  virtual std::expected<void, std::string> LoadDeviceSnapshot() = 0;
  virtual std::expected<void, std::string> InstallSignalHandlers() = 0;
  virtual std::expected<void, std::string> StartDeviceRuntime() = 0;
  virtual void StopAcceptingDeviceMessages() = 0;
  virtual void StopDeviceRuntime() = 0;
  virtual std::expected<void, std::string> CreateMqtt() = 0;
  virtual std::expected<void, std::string> StartMqtt() = 0;
  virtual bool MqttHasTerminalFailure() const = 0;
  virtual bool MqttCallbackStopRequested() const = 0;
  virtual bool ShutdownRequested() const = 0;
  virtual void WaitForNextCheck() = 0;
  virtual void StopMqtt() = 0;
  virtual void Info(std::string_view message) = 0;
  virtual void Error(std::string_view message) = 0;
};

int RunService(RunMode mode, ServiceOperations& operations);

}  // namespace cns::runtime
