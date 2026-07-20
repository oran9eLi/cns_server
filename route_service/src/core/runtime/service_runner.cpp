// 本文件实现两种运行模式共用的可测试进程生命周期编排。
#include "core/runtime/service_runner.hpp"

#include <algorithm>
#include <exception>
#include <stdexcept>

namespace cns::runtime {
namespace {

bool Failed(const std::expected<void, std::string>& result,
            ServiceOperations& operations) {
  if (result) return false;
  operations.Error(result.error());
  return true;
}

}  // namespace

std::expected<void, std::string> StartDeviceRuntimeTransaction(
    const DeviceRuntimeStartOperations& operations) noexcept {
  auto rollback = [&operations] {
    try {
      operations.rollback();
    } catch (...) {
    }
  };
  try {
    if (auto result = operations.configure_handler(); !result) {
      const auto error = result.error();
      rollback();
      return std::unexpected(error);
    }
    if (auto result = operations.start_postgres_thread(); !result) {
      const auto error = result.error();
      rollback();
      return std::unexpected(error);
    }
    if (auto result = operations.start_device_thread(); !result) {
      const auto error = result.error();
      rollback();
      return std::unexpected(error);
    }
    return {};
  } catch (const std::exception&) {
    rollback();
    return std::unexpected("启动设备运行时失败");
  } catch (...) {
    rollback();
    return std::unexpected("启动设备运行时发生未知异常");
  }
}

SelfOwnedRuntimeThread::~SelfOwnedRuntimeThread() {
  if (thread_.joinable()) {
    stop_source_.request_stop();
    thread_.join();
  }
}

void SelfOwnedRuntimeThread::Start(std::shared_ptr<void> runtime_owner,
                                   Task task) {
  if (thread_.joinable()) throw std::logic_error("运行时线程已经启动");
  stop_source_ = std::stop_source{};
  const auto token = stop_source_.get_token();
  thread_ = std::thread(
      [owner = std::move(runtime_owner), task = std::move(task), token] {
        static_cast<void>(owner);
        task(token);
      });
}

void SelfOwnedRuntimeThread::RequestStop() noexcept {
  stop_source_.request_stop();
}

void SelfOwnedRuntimeThread::Join() {
  if (thread_.joinable()) thread_.join();
}

void SelfOwnedRuntimeThread::Detach() noexcept {
  if (thread_.joinable()) thread_.detach();
}

bool SelfOwnedRuntimeThread::Joinable() const noexcept {
  return thread_.joinable();
}

bool DrainDeviceRuntime(
    std::chrono::milliseconds timeout,
    const DeviceRuntimeDrainOperations& operations) noexcept {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  const auto remaining = [&] {
    return std::max(std::chrono::milliseconds{0},
                    std::chrono::ceil<std::chrono::milliseconds>(
                        deadline - std::chrono::steady_clock::now()));
  };
  try {
    const bool input_drained = operations.wait_input_drained(remaining());
    const bool database_idle = input_drained &&
        operations.wait_database_idle(remaining());
    const bool flushed = database_idle && operations.flush_database(remaining());
    if (input_drained && database_idle && flushed) {
      operations.join_threads();
      operations.disable_external_bridge();
      return true;
    }
    operations.report_timeout(operations.pending_devices());
    operations.disable_external_bridge();
    operations.request_stop();
    operations.detach_threads();
    return false;
  } catch (...) {
    try { operations.disable_external_bridge(); } catch (...) {}
    try { operations.request_stop(); } catch (...) {}
    try { operations.detach_threads(); } catch (...) {}
    return false;
  }
}

int RunService(const RunMode mode, ServiceOperations& operations) {
  if (Failed(operations.LoadConfig(), operations)) return 1;
  operations.InitializeLogger();
  if (Failed(operations.DiscoverMigrations(), operations)) return 1;
  if (Failed(operations.ConnectPostgres(), operations)) return 1;
  if (Failed(operations.ReadAppliedMigrations(), operations)) return 1;
  if (Failed(operations.PlanMigrations(mode == RunMode::kMigrateOnly), operations)) {
    return 1;
  }

  if (mode == RunMode::kMigrateOnly) {
    for (std::size_t index = 0; index < operations.PendingMigrationCount(); ++index) {
      if (Failed(operations.ApplyMigration(index), operations)) return 1;
    }
    operations.Info("数据库迁移完成");
    return 0;
  }

  if (Failed(operations.LoadDeviceSnapshot(), operations)) return 1;
  if (Failed(operations.InstallSignalHandlers(), operations)) return 1;
  if (Failed(operations.CreateMqtt(), operations)) return 1;
  if (Failed(operations.StartDeviceRuntime(), operations)) {
    operations.StopAcceptingDeviceMessages();
    static_cast<void>(operations.StopDeviceRuntime(
        std::chrono::milliseconds{5000}));
    return 1;
  }
  if (Failed(operations.StartMqtt(), operations)) {
    operations.StopAcceptingDeviceMessages();
    static_cast<void>(operations.StopDeviceRuntime(std::chrono::milliseconds{5000}));
    operations.StopMqtt();
    return 1;
  }
  operations.Info("路由服务已启动");

  while (true) {
    const bool mqtt_failure = operations.MqttHasTerminalFailure();
    const bool callback_stop = operations.MqttCallbackStopRequested();
    if (mqtt_failure) {
      operations.Error("MQTT后台运行发生终止性失败");
    }
    if (mqtt_failure || callback_stop || operations.ShutdownRequested()) {
      operations.StopAcceptingDeviceMessages();
      static_cast<void>(operations.StopDeviceRuntime(
          std::chrono::milliseconds{5000}));
      operations.StopMqtt();
      if (mqtt_failure) return 1;
      operations.Info("路由服务已停止");
      return 0;
    }
    operations.WaitForNextCheck();
  }
}

}  // namespace cns::runtime
