// 本文件组装路由服务命令行、迁移检查、MQTT 生命周期与信号退出流程。
#include "adapters/mqtt/mqtt_client.hpp"
#include "adapters/postgres/postgres_store.hpp"
#include "core/cli/command_line.hpp"
#include "core/config/app_config.hpp"
#include "core/logging/logger.hpp"
#include "core/migration/migration.hpp"
#include "core/migration/migration_plan.hpp"
#include "core/runtime/shutdown_flag.hpp"

#include <chrono>
#include <csignal>
#include <iostream>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

namespace {

void HandleShutdownSignal(int) {
  cns::runtime::ShutdownFlag::Request();
}

bool InstallSignalHandlers() {
  if (std::signal(SIGINT, HandleShutdownSignal) == SIG_ERR) return false;
  return std::signal(SIGTERM, HandleShutdownSignal) != SIG_ERR;
}

int Fail(std::string_view message) {
  std::cerr << "错误：" << message << '\n';
  return 1;
}

}  // namespace

int main(int argc, char* argv[]) {
  try {
    std::vector<std::string_view> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index) arguments.emplace_back(argv[index]);

    const auto options = cns::cli::ParseCommandLine(arguments);
    if (!options) {
      std::cerr << "错误：" << options.error() << '\n' << cns::cli::Usage();
      return 1;
    }
    if (options->show_help) {
      std::cout << cns::cli::Usage();
      return 0;
    }
    if (options->show_version) {
      std::cout << "route_service " << CNS_ROUTE_SERVICE_VERSION << '\n';
      return 0;
    }

    const auto config = cns::config::LoadAppConfig(options->config_path);
    if (!config) return Fail(config.error());

    cns::logging::Logger logger{config->logging.level, std::cout, std::cerr};
    const auto available =
        cns::migration::DiscoverMigrations(options->migrations_path);
    if (!available) {
      logger.Error(available.error());
      return 1;
    }

    auto store = cns::postgres::PostgresStore::Connect(config->database, logger);
    if (!store) {
      logger.Error(store.error());
      return 1;
    }
    const auto applied = (*store)->ReadAppliedMigrations();
    if (!applied) {
      logger.Error(applied.error());
      return 1;
    }

    const auto mode = options->migrate_only ? cns::migration::Mode::kApply
                                            : cns::migration::Mode::kCheckOnly;
    const auto plan = cns::migration::BuildMigrationPlan(*available, *applied, mode);
    if (!plan) {
      logger.Error(plan.error());
      return 1;
    }

    if (options->migrate_only) {
      for (const auto& migration : plan->pending) {
        const auto result = (*store)->ApplyMigration(migration);
        if (!result) {
          logger.Error(result.error());
          return 1;
        }
      }
      logger.Info("数据库迁移完成");
      return 0;
    }

    cns::runtime::ShutdownFlag::ResetForTesting();
    if (!InstallSignalHandlers()) return Fail("安装退出信号处理器失败");

    auto mqtt = cns::mqtt::MqttClient::Create(config->mqtt, logger);
    if (!mqtt) {
      logger.Error(mqtt.error());
      return 1;
    }
    const auto started = (*mqtt)->Start();
    if (!started) {
      logger.Error(started.error());
      return 1;
    }

    logger.Info("路由服务已启动");
    while (!cns::runtime::ShutdownFlag::Requested()) {
      std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    (*mqtt)->Stop();
    logger.Info("路由服务已停止");
    return 0;
  } catch (const std::exception&) {
    return Fail("路由服务发生未预期异常");
  } catch (...) {
    return Fail("路由服务发生未知异常");
  }
}
