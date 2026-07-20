// 本文件实现两种运行模式共用的可测试进程生命周期编排。
#include "core/runtime/service_runner.hpp"

namespace cns::runtime {
namespace {

bool Failed(const std::expected<void, std::string>& result,
            ServiceOperations& operations) {
  if (result) return false;
  operations.Error(result.error());
  return true;
}

}  // namespace

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

  if (Failed(operations.InstallSignalHandlers(), operations)) return 1;
  if (Failed(operations.CreateMqtt(), operations)) return 1;
  if (Failed(operations.StartMqtt(), operations)) return 1;
  operations.Info("路由服务已启动");

  while (true) {
    if (operations.MqttHasTerminalFailure()) {
      operations.Error("MQTT后台运行发生终止性失败");
      operations.StopMqtt();
      return 1;
    }
    if (operations.ShutdownRequested()) {
      operations.StopMqtt();
      operations.Info("路由服务已停止");
      return 0;
    }
    operations.WaitForNextCheck();
  }
}

}  // namespace cns::runtime
