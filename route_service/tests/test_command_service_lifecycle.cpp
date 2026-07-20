// 本文件验证配置命令的设备ACK、超时与重启恢复生命周期。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <deque>
#include <tuple>
#include <vector>

#include "core/runtime/command_service.hpp"

using namespace std::chrono_literals;

namespace {
constexpr auto kVendor = "A1b2C3d4E5f6G7h8I9j0";
constexpr auto kCommand = "550e8400-e29b-41d4-a716-446655440000";
const auto kNow = std::chrono::system_clock::time_point{1721471400123ms};

cns::command::CommandRecord Active(cns::command::CommandStatus status,
                                   cns::command::TimePoint created = kNow) {
  return {.command_id = kCommand,
          .source_id = "web-console",
          .request_id = "req-1",
          .target_vendor_id = kVendor,
          .request_payload = {{"schema_version", 1}, {"request_id", "req-1"},
              {"target", {{"vendor_id", kVendor}}},
              {"parameters", {{"telemetry_publish_interval_ms", 2000}}}},
          .status = status,
          .error_code = std::nullopt,
          .error_message = std::nullopt,
          .device_ack = std::nullopt,
          .created_at = created,
          .dispatched_at = status == cns::command::CommandStatus::kDispatched
                               ? std::optional{created}
                               : std::nullopt,
          .updated_at = created,
          .completed_at = std::nullopt};
}

struct Harness {
  cns::device::DeviceRegistry devices;
  cns::command::SourceCatalog sources;
  std::deque<std::pair<std::uint64_t, cns::runtime::CommandDatabaseTask>> db;
  std::vector<std::tuple<std::uint64_t, std::string, std::string>> publishes;
  std::vector<nlohmann::json> acks;
  cns::runtime::CommandService service;

  Harness() : service(sources, devices,
      [this](auto id, auto task) { db.emplace_back(id, std::move(task)); return true; },
      [this](auto token, auto topic, auto payload) {
        publishes.emplace_back(token, std::move(topic), std::move(payload));
        return std::expected<void, std::string>{};
      }, [this](auto, auto payload) { acks.push_back(nlohmann::json::parse(payload)); },
      {}, 256, "cns", 15s, std::chrono::days{30}, 1s, 7) {
    REQUIRE(devices.Load({{kVendor, 1, "SEU", "DCDW-001", "v1",
                           cns::device::Status::kOnline, kNow, std::nullopt,
                           std::nullopt, 1}}));
    REQUIRE(sources.Load({{"web-console", cns::command::SourceKind::kHostApp,
                           std::nullopt, true}}));
  }

  void Reply(cns::command::CommandRecord value) {
    REQUIRE_FALSE(db.empty());
    const auto id = db.front().first;
    db.pop_front();
    service.PushDatabaseResult({id, cns::runtime::CommandDatabaseValue{std::move(value)}});
    service.ProcessReady(kNow);
  }
};
}

TEST_CASE("设备成功ACK只在终态持久化后回程") {
  Harness h;
  h.service.LoadActive({Active(cns::command::CommandStatus::kDispatched)});
  REQUIRE(h.service.ActiveCommandCount() == 1);
  REQUIRE(h.service.TryPush({"cns/" + std::string{kVendor} + "/config/ack",
      R"({"command_id":"550e8400-e29b-41d4-a716-446655440000","status":"applied","restart_required":false})", kNow}));
  h.service.ProcessReady(kNow);
  REQUIRE(h.db.size() == 1);
  CHECK(h.acks.empty());
  auto task = std::get<cns::runtime::TransitionCommandTask>(h.db.front().second);
  CHECK(task.expected == cns::command::CommandStatus::kDispatched);
  CHECK(task.desired == cns::command::CommandStatus::kSucceeded);
  auto done = Active(cns::command::CommandStatus::kSucceeded);
  done.device_ack = task.update.device_ack;
  done.completed_at = kNow;
  h.Reply(done);
  CHECK(h.acks.back()["status"] == "succeeded");
  CHECK(h.service.ActiveCommandCount() == 0);
}

TEST_CASE("设备ACK目标不匹配和重复终态不改变命令") {
  Harness h;
  h.service.LoadActive({Active(cns::command::CommandStatus::kDispatched)});
  h.service.TryPush({"cns/Z1b2C3d4E5f6G7h8I9j0/config/ack",
      R"({"command_id":"550e8400-e29b-41d4-a716-446655440000","status":"applied","restart_required":false})", kNow});
  h.service.ProcessReady(kNow);
  CHECK(h.db.empty());
}

TEST_CASE("超时从创建时间计算并通过条件转换落库") {
  Harness h;
  h.service.LoadActive({Active(cns::command::CommandStatus::kDispatched, kNow - 16s)});
  h.service.ProcessReady(kNow);
  REQUIRE(h.db.size() == 1);
  const auto task = std::get<cns::runtime::TransitionCommandTask>(h.db.front().second);
  CHECK(task.desired == cns::command::CommandStatus::kTimeout);
  CHECK(task.update.error_code == "command_timeout");
}

TEST_CASE("恢复pending命令沿用原command_id重发") {
  Harness h;
  h.service.LoadActive({Active(cns::command::CommandStatus::kPending)});
  h.service.ProcessReady(kNow);
  REQUIRE(h.publishes.size() == 1);
  CHECK(nlohmann::json::parse(std::get<2>(h.publishes.front()))["command_id"] == kCommand);

  h.service.SetMqttAvailable(true);
  h.service.ProcessReady(kNow + 1s);
  CHECK(h.publishes.size() == 1);
  h.service.SetMqttAvailable(false);
  h.service.SetMqttAvailable(true);
  h.service.ProcessReady(kNow + 2s);
  CHECK(h.publishes.size() == 2);
}

TEST_CASE("终态清理按周期单实例限量提交") {
  Harness h;
  h.service.ProcessReady(kNow);
  CHECK(h.db.empty());
  h.service.ProcessReady(kNow + 1s);
  REQUIRE(h.db.size() == 1);
  const auto cleanup = std::get<cns::runtime::CleanupCommandsTask>(h.db.front().second);
  CHECK(cleanup.batch_size == 7);
  CHECK(cleanup.before == kNow + 1s - std::chrono::days{30});
  h.service.ProcessReady(kNow + 2s);
  CHECK(h.db.size() == 1);
}

TEST_CASE("恢复集合超过上限时拒绝加载且活动命令计入受理容量") {
  Harness h;
  std::vector<cns::command::CommandRecord> too_many(257,
      Active(cns::command::CommandStatus::kDispatched));
  for (std::size_t index = 0; index < too_many.size(); ++index) {
    too_many[index].command_id = "550e8400-e29b-41d4-a716-" +
        std::string(12 - std::to_string(index).size(), '0') + std::to_string(index);
  }
  h.service.LoadActive(std::move(too_many));
  CHECK(h.service.ActiveCommandCount() == 0);
}
