// 本文件验证可注入进程编排的失败停止、模式隔离与退出顺序。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "core/runtime/service_runner.hpp"

#include <algorithm>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace {

class FakeOperations final : public cns::runtime::ServiceOperations {
 public:
  std::expected<void, std::string> LoadConfig() override {
    return Call("加载配置");
  }
  void InitializeLogger() override { calls.emplace_back("创建日志器"); }
  std::expected<void, std::string> DiscoverMigrations() override {
    return Call("发现迁移");
  }
  std::expected<void, std::string> ConnectPostgres() override {
    return Call("连接数据库");
  }
  std::expected<void, std::string> ReadAppliedMigrations() override {
    return Call("读取版本");
  }
  std::expected<void, std::string> PlanMigrations(bool apply) override {
    calls.emplace_back(apply ? "规划执行" : "规划检查");
    if (fail_at == calls.back()) return std::unexpected("注入失败");
    return {};
  }
  std::size_t PendingMigrationCount() const override { return pending; }
  std::expected<void, std::string> ApplyMigration(std::size_t index) override {
    calls.emplace_back("执行迁移" + std::to_string(index));
    if (fail_apply_index == index) return std::unexpected("迁移失败");
    return {};
  }
  std::expected<void, std::string> InstallSignalHandlers() override {
    return Call("安装信号");
  }
  std::expected<void, std::string> CreateMqtt() override {
    return Call("创建MQTT");
  }
  std::expected<void, std::string> StartMqtt() override {
    return Call("启动MQTT");
  }
  bool MqttHasTerminalFailure() const override { return terminal_failure; }
  bool ShutdownRequested() const override { return shutdown_requested; }
  void WaitForNextCheck() override {
    calls.emplace_back("等待");
    terminal_failure = fail_in_wait;
    shutdown_requested = shutdown_in_wait;
  }
  void StopMqtt() override { calls.emplace_back("停止MQTT"); }
  void Info(std::string_view message) override {
    calls.emplace_back("信息:" + std::string(message));
  }
  void Error(std::string_view message) override {
    calls.emplace_back("错误:" + std::string(message));
  }

  std::expected<void, std::string> Call(std::string name) {
    calls.push_back(std::move(name));
    if (fail_at == calls.back()) return std::unexpected("注入失败");
    return {};
  }

  std::vector<std::string> calls;
  std::string fail_at;
  std::size_t pending = 0;
  std::size_t fail_apply_index = static_cast<std::size_t>(-1);
  mutable bool terminal_failure = false;
  mutable bool shutdown_requested = true;
  bool fail_in_wait = false;
  bool shutdown_in_wait = false;
};

using cns::runtime::RunMode;
using cns::runtime::RunService;

TEST_CASE("启动前置阶段任一失败都停止并返回非零") {
  for (const std::string failure : {"加载配置", "发现迁移", "连接数据库",
                                    "读取版本", "规划检查"}) {
    CAPTURE(failure);
    FakeOperations operations;
    operations.fail_at = failure;
    CHECK(RunService(RunMode::kNormal, operations) == 1);
    CHECK(operations.calls.back() == "错误:注入失败");
  }
}

TEST_CASE("迁移模式逐项执行并在首错停止且绝不创建MQTT") {
  FakeOperations operations;
  operations.pending = 3;
  operations.fail_apply_index = 1;

  CHECK(RunService(RunMode::kMigrateOnly, operations) == 1);
  CHECK(std::ranges::find(operations.calls, "规划执行") != operations.calls.end());
  CHECK(std::ranges::find(operations.calls, "规划检查") == operations.calls.end());
  CHECK(std::ranges::find(operations.calls, "执行迁移0") != operations.calls.end());
  CHECK(std::ranges::find(operations.calls, "执行迁移1") != operations.calls.end());
  CHECK(std::ranges::find(operations.calls, "执行迁移2") == operations.calls.end());
  CHECK(std::ranges::find(operations.calls, "创建MQTT") == operations.calls.end());
}

TEST_CASE("正常模式只检查迁移且绝不执行迁移") {
  FakeOperations operations;
  operations.pending = 2;

  CHECK(RunService(RunMode::kNormal, operations) == 0);
  CHECK(std::ranges::find(operations.calls, "规划检查") != operations.calls.end());
  CHECK(std::ranges::find_if(operations.calls, [](const auto& call) {
          return call.starts_with("执行迁移");
        }) == operations.calls.end());
}

TEST_CASE("信号安装失败时不创建MQTT") {
  FakeOperations operations;
  operations.fail_at = "安装信号";
  CHECK(RunService(RunMode::kNormal, operations) == 1);
  CHECK(std::ranges::find(operations.calls, "创建MQTT") == operations.calls.end());
}

TEST_CASE("MQTT创建或同步启动失败返回非零") {
  for (const std::string failure : {"创建MQTT", "启动MQTT"}) {
    FakeOperations operations;
    operations.fail_at = failure;
    CHECK(RunService(RunMode::kNormal, operations) == 1);
  }
}

TEST_CASE("MQTT后台终止失败停止客户端并返回非零") {
  FakeOperations operations;
  operations.shutdown_requested = false;
  operations.fail_in_wait = true;

  CHECK(RunService(RunMode::kNormal, operations) == 1);
  CHECK(operations.calls[operations.calls.size() - 2].starts_with(
      "错误:MQTT后台运行发生终止性失败"));
  CHECK(operations.calls.back() == "停止MQTT");
}

TEST_CASE("正常退出先停止MQTT再记录停止并返回零") {
  FakeOperations operations;

  CHECK(RunService(RunMode::kNormal, operations) == 0);
  CHECK(operations.calls[operations.calls.size() - 2] == "停止MQTT");
  CHECK(operations.calls.back() == "信息:路由服务已停止");
}

}  // namespace
