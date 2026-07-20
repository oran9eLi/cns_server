// 本文件声明不依赖外部服务实现的进程生命周期编排边界。
#pragma once

#include <cstddef>
#include <chrono>
#include <expected>
#include <functional>
#include <memory>
#include <stop_token>
#include <thread>
#include <string>
#include <string_view>

namespace cns::runtime {

enum class RunMode { kNormal, kMigrateOnly };

struct DeviceRuntimeStartOperations {
  std::function<std::expected<void, std::string>()> configure_handler;
  std::function<std::expected<void, std::string>()> start_postgres_thread;
  std::function<std::expected<void, std::string>()> start_device_thread;
  std::function<void()> rollback;
};

std::expected<void, std::string> StartDeviceRuntimeTransaction(
    const DeviceRuntimeStartOperations& operations) noexcept;

class SelfOwnedRuntimeThread {
 public:
  using Task = std::function<void(std::stop_token)>;
  ~SelfOwnedRuntimeThread();
  SelfOwnedRuntimeThread() = default;
  SelfOwnedRuntimeThread(const SelfOwnedRuntimeThread&) = delete;
  SelfOwnedRuntimeThread& operator=(const SelfOwnedRuntimeThread&) = delete;

  /** 线程入口捕获 runtime_owner，detach 后仍保证任务访问对象存活。 */
  void Start(std::shared_ptr<void> runtime_owner, Task task);
  void RequestStop() noexcept;
  void Join();
  void Detach() noexcept;
  [[nodiscard]] bool Joinable() const noexcept;

 private:
  std::stop_source stop_source_;
  std::thread thread_;
};

struct DeviceRuntimeDrainOperations {
  std::function<bool(std::chrono::milliseconds)> wait_input_drained;
  std::function<bool(std::chrono::milliseconds)> wait_database_idle;
  std::function<bool(std::chrono::milliseconds)> flush_database;
  std::function<std::size_t()> pending_devices;
  std::function<void()> disable_external_bridge;
  std::function<void()> request_stop;
  std::function<void()> join_threads;
  std::function<void()> detach_threads;
  std::function<void(std::size_t)> report_timeout;
};

/** 使用单一总期限完成正常 join，或先禁用外部桥再安全 detach。 */
bool DrainDeviceRuntime(std::chrono::milliseconds timeout,
                        const DeviceRuntimeDrainOperations& operations) noexcept;

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
  virtual std::expected<void, std::string> LoadCommandState() = 0;
  virtual std::expected<void, std::string> InstallSignalHandlers() = 0;
  virtual std::expected<void, std::string> StartDeviceRuntime() = 0;
  virtual void StopAcceptingDeviceMessages() = 0;
  /** 在总期限内协同排空；超时完成安全分离并返回 false。 */
  virtual bool StopDeviceRuntime(std::chrono::milliseconds timeout) = 0;
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
