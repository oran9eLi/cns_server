// 本文件验证可注入进程编排的失败停止、模式隔离与退出顺序。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "core/runtime/service_runner.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;

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
  std::expected<void, std::string> LoadDeviceSnapshot() override {
    return Call("加载设备快照");
  }
  std::expected<void, std::string> LoadCommandState() override {
    return Call("加载命令状态");
  }
  std::expected<void, std::string> StartDeviceRuntime() override {
    return Call("启动设备运行时");
  }
  void StopAcceptingDeviceMessages() override {
    calls.emplace_back("停止接收设备消息");
  }
  bool StopDeviceRuntime(std::chrono::milliseconds timeout) override {
    flush_timeout = timeout;
    calls.emplace_back("排空设备运行时5秒");
    return flush_succeeds;
  }
  std::expected<void, std::string> CreateMqtt() override {
    return Call("创建MQTT");
  }
  std::expected<void, std::string> StartMqtt() override {
    return Call("启动MQTT");
  }
  bool MqttHasTerminalFailure() const override { return terminal_failure; }
  bool MqttCallbackStopRequested() const override {
    return callback_stop_requested;
  }
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
  bool callback_stop_requested = false;
  bool flush_succeeds = true;
  std::chrono::milliseconds flush_timeout{};
};

using cns::runtime::RunMode;
using cns::runtime::RunService;
using cns::runtime::DeviceRuntimeStartOperations;
using cns::runtime::StartDeviceRuntimeTransaction;
using cns::runtime::SelfOwnedRuntimeThread;
using cns::runtime::DeviceRuntimeDrainOperations;
using cns::runtime::DrainDeviceRuntime;

TEST_CASE("设备运行时事务在handler配置失败时回滚且不启动线程") {
  std::vector<std::string> calls;
  DeviceRuntimeStartOperations operations{
      .configure_handler = [&]() -> std::expected<void, std::string> {
        calls.emplace_back("配置handler");
        return std::unexpected("注入失败");
      },
      .start_postgres_thread = [&]() -> std::expected<void, std::string> {
        calls.emplace_back("启动数据库线程");
        return {};
      },
      .start_device_thread = [&]() -> std::expected<void, std::string> {
        calls.emplace_back("启动业务线程");
        return {};
      },
      .rollback = [&] { calls.emplace_back("回滚"); },
  };

  CHECK_FALSE(StartDeviceRuntimeTransaction(operations));
  CHECK(calls == std::vector<std::string>{"配置handler", "回滚"});
}

TEST_CASE("设备运行时事务在第二线程失败时回滚第一线程") {
  std::vector<std::string> calls;
  DeviceRuntimeStartOperations operations{
      .configure_handler = [&]() -> std::expected<void, std::string> {
        calls.emplace_back("配置handler");
        return {};
      },
      .start_postgres_thread = [&]() -> std::expected<void, std::string> {
        calls.emplace_back("启动数据库线程");
        return {};
      },
      .start_device_thread = [&]() -> std::expected<void, std::string> {
        calls.emplace_back("启动业务线程");
        return std::unexpected("注入失败");
      },
      .rollback = [&] { calls.emplace_back("回滚"); },
  };

  CHECK_FALSE(StartDeviceRuntimeTransaction(operations));
  CHECK(calls == std::vector<std::string>{"配置handler", "启动数据库线程",
                                          "启动业务线程", "回滚"});
}

TEST_CASE("设备运行时事务捕获线程构造异常并回滚") {
  std::atomic_int rollbacks{0};
  DeviceRuntimeStartOperations operations{
      .configure_handler = []() -> std::expected<void, std::string> { return {}; },
      .start_postgres_thread = []() -> std::expected<void, std::string> {
        return {};
      },
      .start_device_thread = []() -> std::expected<void, std::string> {
        throw std::runtime_error("注入线程构造异常");
      },
      .rollback = [&] { ++rollbacks; },
  };

  auto result = StartDeviceRuntimeTransaction(operations);
  CHECK_FALSE(result);
  CHECK(result.error() == "启动设备运行时失败");
  CHECK(rollbacks == 1);
}

TEST_CASE("分离线程自持有运行时且外部桥失效后不回调") {
  struct RuntimeOwner {
    explicit RuntimeOwner(std::atomic_bool& destroyed_value)
        : destroyed(destroyed_value) {}
    ~RuntimeOwner() { destroyed = true; }
    std::atomic_bool& destroyed;
  };
  struct ExternalState {
    std::atomic_bool enabled{true};
    std::atomic_int callbacks{0};
  };
  std::atomic_bool release{false};
  std::atomic_bool destroyed{false};
  auto owner = std::make_shared<RuntimeOwner>(destroyed);
  auto external = std::make_shared<ExternalState>();
  SelfOwnedRuntimeThread thread;
  thread.Start(owner, [&, weak = std::weak_ptr<ExternalState>{external}](
                          std::stop_token) {
    while (!release) std::this_thread::yield();
    if (const auto bridge = weak.lock(); bridge && bridge->enabled) {
      ++bridge->callbacks;
    }
  });

  external->enabled = false;
  owner.reset();
  thread.Detach();
  release = true;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds{500};
  while (!destroyed && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  CHECK(destroyed);
  CHECK(external->callbacks == 0);
}

TEST_CASE("真实接线排空编排在链路完成时走join而非detach") {
  std::vector<std::string> calls;
  DeviceRuntimeDrainOperations operations{
      .wait_input_drained = [&](std::chrono::milliseconds) {
        calls.emplace_back("输入排空");
        return true;
      },
      .wait_database_idle = [&](std::chrono::milliseconds) {
        calls.emplace_back("数据库链路完成");
        return true;
      },
      .flush_database = [&](std::chrono::milliseconds) {
        calls.emplace_back("数据库最终排空");
        return true;
      },
      .pending_devices = [] { return std::size_t{0}; },
      .disable_external_bridge = [&] { calls.emplace_back("禁用外部桥"); },
      .request_stop = [&] { calls.emplace_back("请求停止"); },
      .join_threads = [&] { calls.emplace_back("join"); },
      .detach_threads = [&] { calls.emplace_back("detach"); },
      .report_timeout = [](std::size_t) {},
  };

  CHECK(DrainDeviceRuntime(5000ms, operations));
  CHECK(calls == std::vector<std::string>{
                     "输入排空", "数据库链路完成", "数据库最终排空",
                     "join", "禁用外部桥"});
}

TEST_CASE("真实接线排空编排超时禁用桥后分离且不join") {
  std::vector<std::string> calls;
  std::size_t reported = 0;
  DeviceRuntimeDrainOperations operations{
      .wait_input_drained = [](std::chrono::milliseconds) { return false; },
      .wait_database_idle = [](std::chrono::milliseconds) { return true; },
      .flush_database = [](std::chrono::milliseconds) { return true; },
      .pending_devices = [] { return std::size_t{3}; },
      .disable_external_bridge = [&] { calls.emplace_back("禁用外部桥"); },
      .request_stop = [&] { calls.emplace_back("请求停止"); },
      .join_threads = [&] { calls.emplace_back("join"); },
      .detach_threads = [&] { calls.emplace_back("detach"); },
      .report_timeout = [&](std::size_t pending) { reported = pending; },
  };

  CHECK_FALSE(DrainDeviceRuntime(5000ms, operations));
  CHECK(reported == 3);
  CHECK(calls == std::vector<std::string>{"禁用外部桥", "请求停止",
                                          "detach"});
}

TEST_CASE("两个分离线程共同自持有运行时且桥销毁后不回调") {
  struct Owner {
    explicit Owner(std::atomic_bool& destroyed_value) : destroyed(destroyed_value) {}
    ~Owner() { destroyed = true; }
    std::atomic_bool& destroyed;
  };
  struct Bridge { std::atomic_int callbacks{0}; };
  std::atomic_bool release_database{false};
  std::atomic_bool business_finished{false};
  std::atomic_bool destroyed{false};
  auto owner = std::make_shared<Owner>(destroyed);
  auto bridge = std::make_shared<Bridge>();
  SelfOwnedRuntimeThread database;
  SelfOwnedRuntimeThread business;
  database.Start(owner, [&, weak = std::weak_ptr<Bridge>{bridge}](std::stop_token) {
    while (!release_database) std::this_thread::yield();
    if (const auto output = weak.lock()) ++output->callbacks;
  });
  business.Start(owner, [&](std::stop_token) { business_finished = true; });
  while (!business_finished) std::this_thread::yield();
  owner.reset();
  bridge.reset();
  database.Detach();
  business.Detach();
  release_database = true;
  const auto deadline = std::chrono::steady_clock::now() + 500ms;
  while (!destroyed && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  CHECK(destroyed);
}

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
  CHECK(std::ranges::find(operations.calls, "加载设备快照") ==
        operations.calls.end());
  CHECK(std::ranges::find(operations.calls, "启动设备运行时") ==
        operations.calls.end());
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

TEST_CASE("MQTT启动失败仍按设备运行时顺序清理") {
  FakeOperations operations;
  operations.fail_at = "启动MQTT";

  CHECK(RunService(RunMode::kNormal, operations) == 1);
  const auto stop_accepting = std::ranges::find(
      operations.calls, "停止接收设备消息");
  const auto stop_runtime = std::ranges::find(
      operations.calls, "排空设备运行时5秒");
  const auto stop_mqtt = std::ranges::find(operations.calls, "停止MQTT");
  REQUIRE(stop_accepting != operations.calls.end());
  REQUIRE(stop_runtime != operations.calls.end());
  REQUIRE(stop_mqtt != operations.calls.end());
  CHECK(stop_accepting < stop_runtime);
  CHECK(stop_runtime < stop_mqtt);
}

TEST_CASE("设备运行时部分启动失败仍调用幂等回滚") {
  FakeOperations operations;
  operations.fail_at = "启动设备运行时";

  CHECK(RunService(RunMode::kNormal, operations) == 1);
  CHECK(std::ranges::find(operations.calls, "停止接收设备消息") !=
        operations.calls.end());
  CHECK(std::ranges::find(operations.calls, "排空设备运行时5秒") !=
        operations.calls.end());
  CHECK(operations.flush_timeout == std::chrono::milliseconds{5000});
  CHECK(std::ranges::find(operations.calls, "启动MQTT") ==
        operations.calls.end());
}

TEST_CASE("MQTT后台终止失败停止客户端并返回非零") {
  FakeOperations operations;
  operations.shutdown_requested = false;
  operations.fail_in_wait = true;

  CHECK(RunService(RunMode::kNormal, operations) == 1);
  CHECK(std::ranges::find(operations.calls,
                          "错误:MQTT后台运行发生终止性失败") !=
        operations.calls.end());
  CHECK(operations.calls.back() == "停止MQTT");
}

TEST_CASE("正常退出先停止MQTT再记录停止并返回零") {
  FakeOperations operations;

  CHECK(RunService(RunMode::kNormal, operations) == 0);
  const auto stop_accepting = std::ranges::find(
      operations.calls, "停止接收设备消息");
  const auto stop_runtime = std::ranges::find(
      operations.calls, "排空设备运行时5秒");
  const auto stop_mqtt = std::ranges::find(operations.calls, "停止MQTT");
  REQUIRE(stop_accepting != operations.calls.end());
  REQUIRE(stop_runtime != operations.calls.end());
  REQUIRE(stop_mqtt != operations.calls.end());
  CHECK(stop_accepting < stop_runtime);
  CHECK(stop_runtime < stop_mqtt);
  CHECK(operations.flush_timeout == std::chrono::milliseconds{5000});
  CHECK(operations.calls.back() == "信息:路由服务已停止");
}

TEST_CASE("设备运行时排空超时仍继续停止MQTT") {
  FakeOperations operations;
  operations.flush_succeeds = false;

  CHECK(RunService(RunMode::kNormal, operations) == 0);
  CHECK(operations.flush_timeout == std::chrono::milliseconds{5000});
  CHECK(std::ranges::find(operations.calls, "停止MQTT") !=
        operations.calls.end());
}

TEST_CASE("正常启动按迁移检查快照信号设备运行时和MQTT排序") {
  FakeOperations operations;

  CHECK(RunService(RunMode::kNormal, operations) == 0);
  const auto migration = std::ranges::find(operations.calls, "规划检查");
  const auto snapshot = std::ranges::find(operations.calls, "加载设备快照");
  const auto commands = std::ranges::find(operations.calls, "加载命令状态");
  const auto signal = std::ranges::find(operations.calls, "安装信号");
  const auto runtime = std::ranges::find(operations.calls, "启动设备运行时");
  const auto mqtt = std::ranges::find(operations.calls, "启动MQTT");
  REQUIRE(migration != operations.calls.end());
  REQUIRE(snapshot != operations.calls.end());
  REQUIRE(commands != operations.calls.end());
  REQUIRE(signal != operations.calls.end());
  REQUIRE(runtime != operations.calls.end());
  REQUIRE(mqtt != operations.calls.end());
  CHECK(migration < snapshot);
  CHECK(snapshot < commands);
  CHECK(commands < signal);
  CHECK(signal < runtime);
  CHECK(runtime < mqtt);
}

TEST_CASE("MQTT回调停止请求由外部循环执行完整停止") {
  FakeOperations operations;
  operations.shutdown_requested = false;
  operations.callback_stop_requested = true;

  CHECK(RunService(RunMode::kNormal, operations) == 0);
  CHECK(std::ranges::count(operations.calls, "停止MQTT") == 1);
  CHECK(std::ranges::find(operations.calls, "停止接收设备消息") !=
        operations.calls.end());
}

}  // namespace
