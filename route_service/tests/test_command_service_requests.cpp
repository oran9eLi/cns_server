// 本文件验证配置命令的受理、持久化、幂等和发布编排。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <deque>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "core/runtime/command_service.hpp"
#include "core/runtime/command_ingress.hpp"

using namespace std::chrono_literals;

namespace {

constexpr auto kVendor = "A1b2C3d4E5f6G7h8I9j0";
const auto kNow = std::chrono::system_clock::time_point{1721471400123ms};

cns::device::DeviceRecord Device(
    cns::device::Status status = cns::device::Status::kOnline) {
  return {.vendor_id = kVendor,
          .school_id = 1,
          .school_name = "SEU",
          .dcdw_label = "DCDW-001",
          .model_version = "v1",
          .status = status,
          .last_seen_at = kNow,
          .latest_telemetry = std::nullopt,
          .telemetry_received_at = std::nullopt,
          .revision = 1};
}

std::string Request(std::string request_id = "req-1",
                    std::uint32_t interval = 2000) {
  return nlohmann::json{{"schema_version", 1},
                        {"request_id", std::move(request_id)},
                        {"target", {{"vendor_id", kVendor}}},
                        {"parameters",
                         {{"telemetry_publish_interval_ms", interval}}}}
      .dump();
}

std::string ControlRequest(std::string request_id = "control-1",
                           std::string command = "takeoff") {
  return nlohmann::json{{"schema_version", 1},
                        {"request_id", std::move(request_id)},
                        {"target", {{"vendor_id", kVendor}}},
                        {"command", std::move(command)},
                        {"parameters", nlohmann::json::object()}}
      .dump();
}

struct Harness {
  cns::device::DeviceRegistry devices;
  cns::command::SourceCatalog sources;
  std::deque<std::pair<std::uint64_t, cns::runtime::CommandDatabaseTask>> db;
  std::vector<std::tuple<std::uint64_t, std::string, std::string>> device_publish;
  std::vector<std::pair<std::string, nlohmann::json>> source_ack;
  bool accept_database = true;
  cns::runtime::CommandService service;

  explicit Harness(std::size_t capacity = 256,
                   cns::device::Status status = cns::device::Status::kOnline)
      : service(
            sources, devices,
            [this](std::uint64_t operation,
                   cns::runtime::CommandDatabaseTask task) {
              if (!accept_database) return false;
              db.emplace_back(operation, std::move(task));
              return true;
            },
            [this](std::uint64_t token, std::string topic, std::string payload) {
              device_publish.emplace_back(token, std::move(topic),
                                          std::move(payload));
              return std::expected<void, std::string>{};
            },
            [this](std::string topic, std::string payload) {
              source_ack.emplace_back(std::move(topic),
                                      nlohmann::json::parse(payload));
            },
            {}, capacity, "cns") {
    REQUIRE(devices.Load({Device(status)}));
    REQUIRE(sources.Load({{"web-console", cns::command::SourceKind::kHostApp,
                           std::nullopt, true},
                          {"disabled", cns::command::SourceKind::kHostApp,
                           std::nullopt, false}}));
  }

  void Push(std::string payload = Request(),
            std::string source = "web-console") {
    REQUIRE(service.TryPush({.topic = "cns/sources/" + source +
                                      "/config/request",
                             .payload = std::move(payload),
                             .received_at = kNow}));
    service.ProcessReady(kNow);
  }

  void PushControl(std::string payload = ControlRequest(),
                   std::string source = "web-console") {
    REQUIRE(service.TryPush({.topic = "cns/sources/" + source +
                                      "/control/request",
                             .payload = std::move(payload),
                             .received_at = kNow}));
    service.ProcessReady(kNow);
  }

  template <class Value>
  void Reply(Value value) {
    REQUIRE_FALSE(db.empty());
    const auto operation = db.front().first;
    db.pop_front();
    service.PushDatabaseResult(
        {.operation_id = operation,
         .value = cns::runtime::CommandDatabaseValue{std::move(value)}});
    service.ProcessReady(kNow);
  }
};

}  // namespace

TEST_CASE("未登记禁用及不可用状态在入库前拒绝") {
  Harness harness;
  harness.Push(Request(), "unknown");
  REQUIRE(harness.source_ack.size() == 1);
  CHECK(harness.source_ack.back().second["error"]["code"] ==
        "source_not_registered");
  CHECK(harness.source_ack.back().second["command_id"].is_null());
  CHECK(harness.db.empty());

  harness.Push(Request(), "disabled");
  CHECK(harness.source_ack.back().second["error"]["code"] == "source_disabled");

  harness.service.SetMqttAvailable(false);
  harness.Push();
  CHECK(harness.source_ack.back().second["error"]["code"] ==
        "mqtt_unavailable");
  harness.service.SetMqttAvailable(true);
  harness.service.SetDatabaseAvailable(false);
  harness.Push();
  CHECK(harness.source_ack.back().second["error"]["code"] ==
        "database_unavailable");
}

TEST_CASE("非法JSON和非法请求号不创建命令且不记录payload") {
  Harness harness;
  std::vector<std::string> diagnostics;
  harness.service.SetDiagnosticSinkForTesting(
      [&](std::string message) { diagnostics.push_back(std::move(message)); });
  harness.Push("{secret");
  CHECK(harness.source_ack.back().second["request_id"].is_null());
  CHECK(harness.source_ack.back().second["error"]["code"] == "invalid_json");
  CHECK(harness.db.empty());
  for (const auto& message : diagnostics) CHECK(message.find("secret") == std::string::npos);
}

TEST_CASE("可幂等的参数和目标拒绝先查询再保存failed") {
  Harness harness;
  const auto invalid = nlohmann::json{{"schema_version", 1},
                                      {"request_id", "bad-1"},
                                      {"target", {{"vendor_id", kVendor}}},
                                      {"parameters", nlohmann::json::object()}}
                           .dump();
  harness.Push(invalid);
  REQUIRE(std::holds_alternative<cns::runtime::FindCommandTask>(
      harness.db.front().second));
  harness.Reply(std::optional<cns::command::CommandRecord>{});
  REQUIRE(std::holds_alternative<cns::runtime::InsertCommandTask>(
      harness.db.front().second));
  auto record = std::get<cns::runtime::InsertCommandTask>(harness.db.front().second)
                    .command;
  CHECK(record.status == cns::command::CommandStatus::kFailed);
  CHECK(record.error_code == "invalid_parameters");
  harness.Reply(record);
  REQUIRE(harness.source_ack.size() == 1);
  CHECK(harness.source_ack.back().second["status"] == "failed");
  CHECK(harness.device_publish.empty());
}

TEST_CASE("离线目标保存稳定失败而数据库写入失败不发布ACK") {
  Harness offline(256, cns::device::Status::kOffline);
  offline.Push();
  offline.Reply(std::optional<cns::command::CommandRecord>{});
  auto failed = std::get<cns::runtime::InsertCommandTask>(offline.db.front().second)
                    .command;
  CHECK(failed.error_code == "target_offline");
  offline.Reply(failed);
  CHECK(offline.source_ack.back().second["error"]["code"] == "target_offline");

  Harness database_failure;
  database_failure.Push();
  database_failure.Reply(std::optional<cns::command::CommandRecord>{});
  const auto operation = database_failure.db.front().first;
  database_failure.db.pop_front();
  database_failure.service.PushDatabaseResult(
      {.operation_id = operation,
       .value = std::unexpected(cns::runtime::DatabaseError{
           cns::runtime::DatabaseError::Kind::kPermanent, "内部详情"})});
  database_failure.service.ProcessReady(kNow);
  CHECK(database_failure.source_ack.empty());
  CHECK(database_failure.device_publish.empty());
}

TEST_CASE("成功命令严格经过Find Insert Publish MID Transition再回程") {
  Harness harness;
  harness.Push();
  REQUIRE(std::holds_alternative<cns::runtime::FindCommandTask>(
      harness.db.front().second));
  harness.Reply(std::optional<cns::command::CommandRecord>{});
  REQUIRE(std::holds_alternative<cns::runtime::InsertCommandTask>(
      harness.db.front().second));
  auto pending = std::get<cns::runtime::InsertCommandTask>(harness.db.front().second)
                     .command;
  CHECK(pending.status == cns::command::CommandStatus::kPending);
  harness.Reply(pending);
  REQUIRE(harness.device_publish.size() == 1);
  CHECK(harness.db.empty());
  CHECK(harness.source_ack.empty());

  const auto token = std::get<0>(harness.device_publish.front());
  harness.service.PushPublishCompletion(
      {.token = token, .result = std::expected<void, std::string>{}});
  harness.service.ProcessReady(kNow);
  REQUIRE(std::holds_alternative<cns::runtime::TransitionCommandTask>(
      harness.db.front().second));
  auto dispatched = pending;
  dispatched.status = cns::command::CommandStatus::kDispatched;
  dispatched.dispatched_at = kNow;
  harness.Reply(dispatched);
  REQUIRE(harness.source_ack.size() == 1);
  CHECK(harness.source_ack.back().second["status"] == "dispatched");
  CHECK(harness.source_ack.back().second["command_id"] == pending.command_id);
}

TEST_CASE("相同请求重放当前ACK而内容冲突不再次发布") {
  Harness harness;
  harness.Push();
  auto existing = cns::command::CommandRecord{
      .command_id = "550e8400-e29b-41d4-a716-446655440000",
      .source_id = "web-console",
      .request_id = "req-1",
      .target_vendor_id = kVendor,
      .request_payload = nlohmann::json::parse(Request()),
      .status = cns::command::CommandStatus::kDispatched,
      .error_code = std::nullopt,
      .error_message = std::nullopt,
      .device_ack = std::nullopt,
      .created_at = kNow,
      .dispatched_at = kNow,
      .updated_at = kNow,
      .completed_at = std::nullopt};
  harness.Reply(std::optional<cns::command::CommandRecord>{existing});
  REQUIRE(harness.source_ack.size() == 1);
  CHECK(harness.source_ack.back().second["status"] == "dispatched");
  CHECK(harness.device_publish.empty());

  harness.Push(Request("req-1", 3000));
  harness.Reply(std::optional<cns::command::CommandRecord>{existing});
  CHECK(harness.source_ack.back().second["error"]["code"] ==
        "idempotency_conflict");
  CHECK(harness.device_publish.empty());
}

TEST_CASE("飞控请求保存类型并通过飞控主题发布") {
  Harness harness;
  harness.PushControl();
  REQUIRE(std::holds_alternative<cns::runtime::FindCommandTask>(
      harness.db.front().second));
  harness.Reply(std::optional<cns::command::CommandRecord>{});
  auto pending = std::get<cns::runtime::InsertCommandTask>(harness.db.front().second)
                     .command;
  CHECK(pending.command_type == cns::command::CommandType::kControl);
  harness.Reply(pending);
  REQUIRE(harness.device_publish.size() == 1);
  CHECK(std::get<1>(harness.device_publish.front()) ==
        std::string{"cns/"} + kVendor + "/control/set");
  const auto payload = nlohmann::json::parse(
      std::get<2>(harness.device_publish.front()));
  CHECK(payload["command"] == "takeoff");
  CHECK(payload["command_id"] == pending.command_id);
}

TEST_CASE("跨配置和飞控类型复用请求号属于幂等冲突") {
  Harness harness;
  harness.PushControl(ControlRequest("req-1"));
  auto existing = cns::command::CommandRecord{
      .command_id = "550e8400-e29b-41d4-a716-446655440000",
      .source_id = "web-console", .request_id = "req-1",
      .target_vendor_id = kVendor,
      .request_payload = nlohmann::json::parse(Request("req-1")),
      .status = cns::command::CommandStatus::kDispatched,
      .error_code = std::nullopt, .error_message = std::nullopt,
      .device_ack = std::nullopt, .created_at = kNow,
      .dispatched_at = kNow, .updated_at = kNow,
      .completed_at = std::nullopt};
  harness.Reply(std::optional<cns::command::CommandRecord>{existing});
  REQUIRE(harness.source_ack.size() == 1);
  CHECK(harness.source_ack.back().second["error"]["code"] ==
        "idempotency_conflict");
  CHECK(harness.device_publish.empty());
}

TEST_CASE("在途上限拒绝第二个请求且不提交数据库") {
  Harness harness(1);
  harness.Push(Request("req-1"));
  REQUIRE(harness.db.size() == 1);
  harness.Push(Request("req-2"));
  CHECK(harness.db.size() == 1);
  CHECK(harness.source_ack.back().second["error"]["code"] == "server_busy");
}

TEST_CASE("数据库唯一约束竞态返回既有记录时不重复发布") {
  Harness harness;
  harness.Push();
  harness.Reply(std::optional<cns::command::CommandRecord>{});
  const auto candidate =
      std::get<cns::runtime::InsertCommandTask>(harness.db.front().second).command;
  auto existing = candidate;
  existing.command_id = "550e8400-e29b-41d4-a716-446655440000";
  existing.status = cns::command::CommandStatus::kDispatched;
  existing.dispatched_at = kNow;
  harness.Reply(existing);

  REQUIRE(harness.source_ack.size() == 1);
  CHECK(harness.source_ack.back().second["command_id"] == existing.command_id);
  CHECK(harness.device_publish.empty());
}

TEST_CASE("命令入口只分派命令topic且拒绝诊断按三十秒限频") {
  Harness harness;
  std::vector<std::string> diagnostics;
  auto steady_now = std::chrono::steady_clock::time_point{};
  cns::runtime::CommandIngress ingress(
      harness.service, "cns",
      [&](std::string message) { diagnostics.push_back(std::move(message)); },
      [&] { return steady_now; });

  CHECK_FALSE(ingress.TryPush({.topic = "cns/device/telemetry",
                               .payload = "{}",
                               .received_at = kNow}));
  CHECK(ingress.TryPush({.topic = "cns/sources/web-console/control/request",
                         .payload = ControlRequest(), .received_at = kNow}));
  harness.service.Close();
  CHECK_FALSE(ingress.TryPush({.topic = "cns/sources/web-console/config/request",
                               .payload = Request(),
                               .received_at = kNow}));
  CHECK_FALSE(ingress.TryPush({.topic = "cns/sources/web-console/config/request",
                               .payload = Request(),
                               .received_at = kNow}));
  REQUIRE(diagnostics.size() == 1);
  steady_now += 30s;
  CHECK_FALSE(ingress.TryPush({.topic = "cns/sources/web-console/config/request",
                               .payload = Request(),
                               .received_at = kNow}));
  CHECK(diagnostics.size() == 2);

  ingress.Disable();
}
