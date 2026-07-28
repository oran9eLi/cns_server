#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "core/runtime/device_service.hpp"
#include "core/runtime/postgres_worker.hpp"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <future>
#include <latch>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
namespace {
using cns::device::DeviceRecord;
using cns::device::Status;
using cns::persistence::DesiredDeviceWrite;
using cns::protocol::Registration;
using cns::protocol::RegistrationStatus;
using cns::runtime::DatabaseResult;
using cns::runtime::DeviceService;
using cns::runtime::PostgresWorker;
constexpr auto kKnown = "A1b2C3d4E5f6G7h8I9j0";
constexpr auto kNew = "Z9y8X7w6V5u4T3s2R1q0";
constexpr auto kOther = "M1n2B3v4C5x6Z7a8S9d0";

std::string RegistrationJson(std::string_view vendor,
                             std::string_view school = "SEU",
                             std::string_view label = "DCDW-001") {
  return nlohmann::json{{"schema_version", 1}, {"vendor_id", vendor},
                        {"status", "online"}, {"school_name", school},
                        {"dcdw_label", label}}.dump();
}

DeviceRecord Record(std::string vendor, std::uint64_t revision = 1) {
  return {.vendor_id = std::move(vendor), .school_id = 7,
          .school_name = "SEU", .dcdw_label = "DCDW-001",
          .model_version = "v1", .status = Status::kOffline,
          .last_seen_at = std::nullopt, .latest_telemetry = std::nullopt,
          .telemetry_received_at = std::nullopt, .revision = revision};
}

cns::mqtt::InboundMessage Message(std::string topic, std::string payload,
                                  cns::runtime::TimePoint at = {}) {
  return {std::move(topic), std::move(payload), at};
}

bool WaitUntil(const std::function<bool()>& predicate,
               std::chrono::milliseconds timeout = 500ms);

struct Harness {
  cns::device::DeviceRegistry registry;
  std::vector<Registration> provisions;
  std::vector<DesiredDeviceWrite> writes;
  std::vector<cns::runtime::PublishedState> events;
  std::vector<std::vector<cns::runtime::PublishedState>> snapshots;
  std::vector<std::string> diagnostics;
  std::vector<std::string> information;
  std::chrono::steady_clock::time_point steady{};

  DeviceService Make(std::size_t capacity = 8) {
    return DeviceService(
        registry,
        [this](Registration r, cns::runtime::TimePoint) -> bool {
          provisions.push_back(std::move(r));
          return true;
        },
        [this](DesiredDeviceWrite w) -> bool {
          writes.push_back(std::move(w));
          return true;
        },
        [this](cns::runtime::PublishedState e) { events.push_back(std::move(e)); },
        [this] { return steady; },
        [this](std::string error) { diagnostics.push_back(std::move(error)); },
        capacity, "cns", 5s, 60s,
        [this](std::string message) {
          information.push_back(std::move(message));
        },
        [this](std::vector<cns::runtime::PublishedState> snapshot) {
          snapshots.push_back(std::move(snapshot));
        });
  }
};

TEST_CASE("MQTT queue is bounded and close rejects input") {
  Harness h;
  auto service = h.Make(1);
  CHECK(service.TryPush(Message("cns/a/registration", "{}")));
  CHECK_FALSE(service.TryPush(Message("cns/b/registration", "{}")));
  service.Close();
  CHECK_FALSE(service.TryPush(Message("cns/c/registration", "{}")));
}

TEST_CASE("unknown registration is pending and latest candidate wins") {
  Harness h;
  auto service = h.Make();
  REQUIRE(service.TryPush(Message(std::string{"cns/"} + kNew + "/registration",
                                  RegistrationJson(kNew, "old"))));
  REQUIRE(service.TryPush(Message(std::string{"cns/"} + kNew + "/registration",
                                  RegistrationJson(kNew, "SEU", "DCDW-002"))));
  service.ProcessReady(cns::runtime::TimePoint{});
  REQUIRE(h.provisions.size() == 1);
  CHECK(h.provisions.back().school_name == "SEU");
  CHECK(service.PendingRegistrationCount() == 1);
}

TEST_CASE("provision result installs device and applies latest registration") {
  Harness h;
  auto service = h.Make();
  service.TryPush(Message(std::string{"cns/"} + kNew + "/registration",
                          RegistrationJson(kNew, "SEU", "DCDW-002")));
  service.ProcessReady(cns::runtime::TimePoint{});
  service.PushDatabaseResult({DatabaseResult::Kind::kProvisioned, kNew, 1,
                              Record(kNew), {}});
  service.ProcessReady();
  REQUIRE(h.registry.Find(kNew));
  CHECK(h.registry.Find(kNew)->status == Status::kOnline);
  CHECK(service.PendingRegistrationCount() == 0);
  CHECK_FALSE(h.writes.empty());
}

TEST_CASE("closed business service waits for provision and follow-up write results") {
  cns::device::DeviceRegistry registry;
  std::atomic_int provisions{0};
  std::atomic_int writes{0};
  DeviceService service(
      registry,
      [&](Registration, cns::runtime::TimePoint) {
        ++provisions;
        return true;
      },
      [&](DesiredDeviceWrite) {
        ++writes;
        return true;
      },
      [](cns::runtime::PublishedState) {},
      [] { return std::chrono::steady_clock::time_point{}; });
  REQUIRE(service.TryPush(Message(std::string{"cns/"} + kNew + "/registration",
                                  RegistrationJson(kNew, "SEU", "DCDW-001"))));
  service.Close();
  auto run = std::async(std::launch::async, [&] {
    service.Run(std::stop_token{});
  });
  REQUIRE(WaitUntil([&] { return provisions.load() == 1; }));
  CHECK(run.wait_for(20ms) == std::future_status::timeout);

  service.PushDatabaseResult({DatabaseResult::Kind::kProvisioned, kNew, 1,
                              Record(kNew), {}});
  REQUIRE(WaitUntil([&] { return writes.load() == 1; }));
  CHECK(run.wait_for(20ms) == std::future_status::timeout);

  service.PushDatabaseResult({DatabaseResult::Kind::kWriteCompleted, kNew, 2,
                              std::nullopt, {}});
  CHECK(run.wait_for(500ms) == std::future_status::ready);
}

TEST_CASE("关闭时强制写入不足批次间隔的最后 telemetry 并等待结果") {
  cns::device::DeviceRegistry registry;
  REQUIRE(registry.Load({Record(kKnown)}));
  std::mutex writes_mutex;
  std::vector<DesiredDeviceWrite> writes;
  DeviceService service(
      registry, [](Registration, cns::runtime::TimePoint) { return true; },
      [&](DesiredDeviceWrite write) {
        std::lock_guard lock(writes_mutex);
        writes.push_back(std::move(write));
        return true;
      },
      [](cns::runtime::PublishedState) {},
      [] { return std::chrono::steady_clock::time_point{}; });
  REQUIRE(service.TryPush(Message(
      std::string{"cns/"} + kKnown + "/telemetry", R"({"sequence":99})")));
  service.Close();
  auto run = std::async(std::launch::async,
                        [&] { service.Run(std::stop_token{}); });

  REQUIRE(WaitUntil([&] {
    std::lock_guard lock(writes_mutex);
    return !writes.empty();
  }));
  {
    std::lock_guard lock(writes_mutex);
    REQUIRE(writes.size() == 1);
    CHECK(writes.front().write_telemetry);
    CHECK(writes.front().record.latest_telemetry->at("sequence") == 99);
  }
  CHECK(run.wait_for(20ms) == std::future_status::timeout);
  service.PushDatabaseResult({DatabaseResult::Kind::kWriteCompleted, kKnown, 2,
                              std::nullopt, {}});
  CHECK(run.wait_for(500ms) == std::future_status::ready);
}

TEST_CASE("停机强制写入提交失败后不无限重复且保持非空闲") {
  cns::device::DeviceRegistry registry;
  REQUIRE(registry.Load({Record(kKnown)}));
  std::atomic_int attempts{0};
  DeviceService service(
      registry, [](Registration, cns::runtime::TimePoint) { return true; },
      [&](DesiredDeviceWrite) {
        ++attempts;
        return false;
      },
      [](cns::runtime::PublishedState) {},
      [] { return std::chrono::steady_clock::time_point{}; });
  REQUIRE(service.TryPush(Message(
      std::string{"cns/"} + kKnown + "/telemetry", R"({"sequence":7})")));
  service.Close();
  auto run = std::async(std::launch::async,
                        [&] { service.Run(std::stop_token{}); });
  REQUIRE(WaitUntil([&] { return attempts.load() >= 1; }));
  std::this_thread::sleep_for(250ms);
  CHECK(attempts == 1);
  CHECK_FALSE(service.WaitForDatabaseIdle(20ms));
  CHECK(run.wait_for(20ms) == std::future_status::timeout);
  service.CancelOutstandingDatabaseWork();
  CHECK(run.wait_for(500ms) == std::future_status::ready);
}

TEST_CASE("database idle waiter wakes immediately after chained processing finishes") {
  cns::device::DeviceRegistry registry;
  std::latch write_entered{1};
  std::latch release_write{1};
  DeviceService service(
      registry,
      [](Registration, cns::runtime::TimePoint) { return true; },
      [&](DesiredDeviceWrite) {
        write_entered.count_down();
        release_write.wait();
        return true;
      },
      [](cns::runtime::PublishedState) {},
      [] { return std::chrono::steady_clock::time_point{}; });
  REQUIRE(service.TryPush(Message(std::string{"cns/"} + kNew + "/registration",
                                  RegistrationJson(kNew, "SEU", "DCDW-002"))));
  service.ProcessReady();
  service.PushDatabaseResult({DatabaseResult::Kind::kProvisioned, kNew, 1,
                              Record(kNew), {}});
  auto processing = std::async(std::launch::async, [&] { service.ProcessReady(); });
  write_entered.wait();
  auto idle = std::async(std::launch::async, [&] {
    const auto started = std::chrono::steady_clock::now();
    const bool result = service.WaitForDatabaseIdle(2s);
    return std::pair{result, std::chrono::steady_clock::now() - started};
  });
  release_write.count_down();
  processing.get();
  service.PushDatabaseResult({DatabaseResult::Kind::kWriteCompleted, kNew, 2,
                              std::nullopt, {}});
  service.ProcessReady();
  const auto [idle_result, elapsed] = idle.get();
  CHECK(idle_result);
  CHECK(elapsed < 500ms);
}

TEST_CASE("database idle transition cannot be lost between predicate and wait") {
  cns::device::DeviceRegistry registry;
  DeviceService service(
      registry,
      [](Registration, cns::runtime::TimePoint) { return true; },
      [](DesiredDeviceWrite) { return true; },
      [](cns::runtime::PublishedState) {},
      [] { return std::chrono::steady_clock::time_point{}; });
  REQUIRE(service.TryPush(Message(std::string{"cns/"} + kNew + "/registration",
                                  RegistrationJson(kNew))));
  service.ProcessReady();

  std::latch predicate_checked{1};
  std::latch permit_wait{1};
  service.SetBeforeDatabaseIdleWaitHookForTesting([&] {
    predicate_checked.count_down();
    permit_wait.wait();
  });
  auto waiter = std::async(std::launch::async, [&] {
    const auto started = std::chrono::steady_clock::now();
    const bool result = service.WaitForDatabaseIdle(2s);
    return std::pair{result, std::chrono::steady_clock::now() - started};
  });
  predicate_checked.wait();
  service.PushDatabaseResult({DatabaseResult::Kind::kUnavailable, kNew, 0,
                              std::nullopt, {}});
  auto completion = std::async(std::launch::async, [&] { service.ProcessReady(); });
  permit_wait.count_down();
  completion.get();
  const auto [idle_result, elapsed] = waiter.get();
  CHECK(idle_result);
  CHECK(elapsed < 500ms);
}

TEST_CASE("Close在数据库空闲检查到等待窗口内不能丢失唤醒") {
  cns::device::DeviceRegistry registry;
  DeviceService service(
      registry,
      [](Registration, cns::runtime::TimePoint) { return true; },
      [](DesiredDeviceWrite) { return true; },
      [](cns::runtime::PublishedState) {},
      [] { return std::chrono::steady_clock::time_point{}; });
  REQUIRE(service.TryPush(Message(std::string{"cns/"} + kNew + "/registration",
                                  RegistrationJson(kNew))));
  service.ProcessReady();

  std::latch predicate_checked{1};
  std::latch permit_wait{1};
  service.SetBeforeDatabaseIdleWaitHookForTesting([&] {
    predicate_checked.count_down();
    permit_wait.wait();
  });
  auto waiter = std::async(std::launch::async,
                           [&] { return service.WaitForDatabaseIdle(100ms); });
  predicate_checked.wait();
  auto closer = std::async(std::launch::async, [&] { service.Close(); });
  CHECK(closer.wait_for(20ms) == std::future_status::timeout);
  permit_wait.count_down();
  CHECK(closer.wait_for(100ms) == std::future_status::ready);
  CHECK_FALSE(waiter.get());
  service.CancelOutstandingDatabaseWork();
  service.ProcessReady();
}

TEST_CASE("database results have priority over MQTT and unavailable clears pending") {
  Harness h;
  auto service = h.Make();
  service.TryPush(Message(std::string{"cns/"} + kNew + "/registration",
                          RegistrationJson(kNew)));
  service.PushDatabaseResult({DatabaseResult::Kind::kUnavailable, {}, 0,
                              std::nullopt, "down"});
  service.ProcessReady();
  CHECK(service.IsDatabaseUnavailable());
  CHECK(service.PendingRegistrationCount() == 0);
  CHECK(h.provisions.empty());
}

TEST_CASE("database results are never dropped by MQTT queue capacity") {
  Harness h;
  auto service = h.Make(1);
  service.PushDatabaseResult({DatabaseResult::Kind::kUnavailable, {}, 0,
                              std::nullopt, "down"});
  service.PushDatabaseResult({DatabaseResult::Kind::kRecovered, {}, 0,
                              std::nullopt, {}});
  service.ProcessReady();
  CHECK_FALSE(service.IsDatabaseUnavailable());
}

TEST_CASE("existing device updates immediately and telemetry waits five seconds") {
  Harness h;
  auto online = Record(kKnown);
  online.status = Status::kOnline;
  REQUIRE(h.registry.Load({online}));
  auto service = h.Make();
  service.TryPush(Message(std::string{"cns/"} + kKnown + "/telemetry",
                          R"({"temperature":20})"));
  service.ProcessReady(cns::runtime::TimePoint{} + 1s);
  CHECK(h.writes.empty());
  CHECK_FALSE(h.events.empty());
  CHECK_FALSE(h.events.back().degraded);
  h.steady += 5s;
  service.ProcessReady(cns::runtime::TimePoint{} + 2s);
  REQUIRE(h.writes.size() == 1);
  CHECK(h.writes[0].write_telemetry);
  CHECK_FALSE(h.writes[0].write_status);
}

TEST_CASE("外部快照请求只在业务线程发布排序后的全部设备") {
  Harness h;
  auto online_b = Record(kNew, 5);
  online_b.dcdw_label = "DCDW-002";
  online_b.status = Status::kOnline;
  online_b.latest_telemetry = nlohmann::json{{"alarms", nlohmann::json::array()}};
  auto online_a = Record(kKnown, 7);
  online_a.status = Status::kOnline;
  auto offline = Record(kOther, 9);
  offline.dcdw_label = "DCDW-003";
  REQUIRE(h.registry.Load({online_b, offline, online_a}));
  auto service = h.Make();

  service.RequestExternalSnapshot();
  service.RequestExternalSnapshot();
  CHECK(h.snapshots.empty());
  service.ProcessReady();

  REQUIRE(h.snapshots.size() == 1);
  REQUIRE(h.snapshots.front().size() == 3);
  CHECK(h.snapshots.front()[0].record.vendor_id == kKnown);
  CHECK(h.snapshots.front()[1].record.vendor_id == kOther);
  CHECK(h.snapshots.front()[1].record.status == Status::kOffline);
  CHECK(h.snapshots.front()[2].record.vendor_id == kNew);
  CHECK(h.snapshots.front()[2].record.latest_telemetry ==
        online_b.latest_telemetry);
  for (const auto& state : h.snapshots.front()) {
    CHECK(state.reason == cns::state_event::ChangeReason::kSnapshotReplay);
  }
}

TEST_CASE("外部全量快照保留单设备降级状态") {
  Harness h;
  auto online = Record(kKnown, 7);
  online.status = Status::kOnline;
  REQUIRE(h.registry.Load({online}));
  auto service = h.Make();

  service.PushDatabaseResult(
      {DatabaseResult::Kind::kPermanentFailure, kKnown, 7, std::nullopt,
       "测试数据库故障"});
  service.ProcessReady();
  REQUIRE(service.IsDeviceDegraded(kKnown));

  service.RequestExternalSnapshot();
  service.ProcessReady();

  REQUIRE(h.snapshots.size() == 1);
  REQUIRE(h.snapshots.front().size() == 1);
  CHECK(h.snapshots.front().front().record.vendor_id == kKnown);
  CHECK(h.snapshots.front().front().degraded);
}

TEST_CASE("telemetry恢复显式离线设备时同时持久化在线状态") {
  Harness h;
  auto online = Record(kKnown);
  online.status = Status::kOnline;
  online.last_seen_at = cns::runtime::TimePoint{} + 10s;
  REQUIRE(h.registry.Load({online}));
  auto service = h.Make();
  const auto offline = nlohmann::json{{"schema_version", 1},
                                      {"vendor_id", kKnown},
                                      {"status", "offline"}}.dump();
  service.TryPush(Message(std::string{"cns/"} + kKnown + "/registration",
                          offline));
  service.ProcessReady(cns::runtime::TimePoint{} + 1s);
  REQUIRE(h.writes.size() == 1);
  CHECK(h.writes.front().write_status);
  CHECK(h.writes.front().record.status == Status::kOffline);
  CHECK(h.writes.front().record.last_seen_at ==
        cns::runtime::TimePoint{} + 10s);
  service.PushDatabaseResult({DatabaseResult::Kind::kWriteCompleted, kKnown, 2,
                              std::nullopt, {}});
  service.ProcessReady(cns::runtime::TimePoint{} + 1s);

  service.TryPush(Message(std::string{"cns/"} + kKnown + "/telemetry",
                          R"({"temperature":20})"));
  service.ProcessReady(cns::runtime::TimePoint{} + 2s);
  h.steady += 5s;
  service.ProcessReady(cns::runtime::TimePoint{} + 3s);

  REQUIRE(h.writes.size() == 2);
  CHECK(h.writes.back().write_telemetry);
  CHECK(h.writes.back().write_status);
}

TEST_CASE("offline scan runs each second and writes status immediately") {
  Harness h;
  auto online = Record(kKnown);
  online.status = Status::kOnline;
  online.last_seen_at = cns::runtime::TimePoint{};
  REQUIRE(h.registry.Load({online}));
  auto service = h.Make();
  h.steady += 1s;
  service.ProcessReady(cns::runtime::TimePoint{} + 61s);
  REQUIRE(h.writes.size() == 1);
  CHECK(h.writes[0].write_status);
  CHECK(h.registry.Find(kKnown)->status == Status::kOffline);
}

TEST_CASE("设备上线和离线状态持久化完成后输出信息日志") {
  Harness h;
  REQUIRE(h.registry.Load({Record(kKnown)}));
  auto service = h.Make();

  service.TryPush(Message(std::string{"cns/"} + kKnown + "/registration",
                          RegistrationJson(kKnown)));
  service.ProcessReady(cns::runtime::TimePoint{} + 1s);
  REQUIRE(h.writes.size() == 1);
  CHECK(h.information.empty());
  service.PushDatabaseResult({DatabaseResult::Kind::kWriteCompleted, kKnown,
                              h.writes.back().revision, std::nullopt, {}});
  service.ProcessReady(cns::runtime::TimePoint{} + 1s);
  REQUIRE(h.information.size() == 1);
  CHECK(h.information.back() ==
        std::string{"设备上线：SEU/DCDW-001 vendor_id="} + kKnown);

  h.steady += 1s;
  service.ProcessReady(cns::runtime::TimePoint{} + 62s);
  REQUIRE(h.writes.size() == 2);
  CHECK(h.information.size() == 1);
  service.PushDatabaseResult({DatabaseResult::Kind::kWriteCompleted, kKnown,
                              h.writes.back().revision, std::nullopt, {}});
  service.ProcessReady(cns::runtime::TimePoint{} + 62s);
  REQUIRE(h.information.size() == 2);
  CHECK(h.information.back() ==
        std::string{"设备离线：SEU/DCDW-001 vendor_id="} + kKnown);
}

TEST_CASE("database failure degrades events and recovery uses revisions") {
  Harness h;
  REQUIRE(h.registry.Load({Record(kKnown)}));
  auto service = h.Make();
  service.PushDatabaseResult({DatabaseResult::Kind::kUnavailable, {}, 0,
                              std::nullopt, "down"});
  service.ProcessReady();
  service.TryPush(Message(std::string{"cns/"} + kKnown + "/telemetry",
                          R"({"temperature":21})"));
  service.ProcessReady();
  REQUIRE(h.events.back().degraded);
  const auto revision = h.registry.Find(kKnown)->revision;
  service.PushDatabaseResult({DatabaseResult::Kind::kRecovered, {}, 0,
                              std::nullopt, {}});
  service.ProcessReady();
  REQUIRE_FALSE(h.writes.empty());
  service.PushDatabaseResult({DatabaseResult::Kind::kWriteCompleted, kKnown,
                              revision - 1, std::nullopt, {}});
  service.ProcessReady();
  CHECK(service.IsDeviceDegraded(kKnown));
  service.PushDatabaseResult({DatabaseResult::Kind::kWriteCompleted, kKnown,
                              revision, std::nullopt, {}});
  service.ProcessReady();
  CHECK_FALSE(service.IsDeviceDegraded(kKnown));
  CHECK(h.events.back().reason ==
        cns::state_event::ChangeReason::kDatabaseRecovered);
}

TEST_CASE("first unavailable write immediately publishes degraded snapshot") {
  Harness h;
  REQUIRE(h.registry.Load({Record(kKnown)}));
  auto service = h.Make();
  service.TryPush(Message(std::string{"cns/"} + kKnown + "/telemetry",
                          R"({"temperature":22})"));
  service.ProcessReady(cns::runtime::TimePoint{});
  h.steady += 5s;
  service.ProcessReady(cns::runtime::TimePoint{});
  const auto revision = h.registry.Find(kKnown)->revision;
  service.PushDatabaseResult({DatabaseResult::Kind::kUnavailable, kKnown,
                              revision, std::nullopt, "数据库暂不可用"});
  service.ProcessReady(cns::runtime::TimePoint{});
  REQUIRE(h.events.back().record.revision == revision);
  CHECK(h.events.back().reason == cns::state_event::ChangeReason::kTelemetry);
  CHECK(h.events.back().degraded);
}

TEST_CASE("mutation after recovered raises degraded revision") {
  Harness h;
  REQUIRE(h.registry.Load({Record(kKnown)}));
  auto service = h.Make();
  service.PushDatabaseResult({DatabaseResult::Kind::kUnavailable, kKnown, 1,
                              std::nullopt, "数据库暂不可用"});
  service.ProcessReady();
  service.PushDatabaseResult({DatabaseResult::Kind::kRecovered, {}, 0,
                              std::nullopt, {}});
  service.ProcessReady();
  service.TryPush(Message(std::string{"cns/"} + kKnown + "/telemetry",
                          R"({"temperature":23})"));
  service.ProcessReady();
  const auto newer = h.registry.Find(kKnown)->revision;
  REQUIRE(h.events.back().degraded);
  service.PushDatabaseResult({DatabaseResult::Kind::kWriteCompleted, kKnown,
                              newer - 1, std::nullopt, {}});
  service.ProcessReady();
  CHECK(service.IsDeviceDegraded(kKnown));
}

TEST_CASE("inflight provision keeps latest registration without resubmit") {
  Harness h;
  auto service = h.Make();
  service.TryPush(Message(std::string{"cns/"} + kNew + "/registration",
                          RegistrationJson(kNew, "SEU", "DCDW-001")));
  service.ProcessReady();
  service.TryPush(Message(std::string{"cns/"} + kNew + "/registration",
                          RegistrationJson(kNew, "SEU", "DCDW-002")));
  service.ProcessReady();
  REQUIRE(h.provisions.size() == 1);
  service.PushDatabaseResult({DatabaseResult::Kind::kProvisioned, kNew, 1,
                              Record(kNew), {}});
  service.ProcessReady();
  REQUIRE(h.registry.Find(kNew));
  CHECK(h.registry.Find(kNew)->dcdw_label == "DCDW-002");
  REQUIRE_FALSE(h.writes.empty());
  CHECK(h.writes.back().write_metadata);
  CHECK(h.writes.back().write_status);
}

TEST_CASE("closed write submitter retains state and publishes degraded snapshot") {
  cns::device::DeviceRegistry registry;
  REQUIRE(registry.Load({Record(kKnown)}));
  std::vector<cns::runtime::PublishedState> events;
  std::vector<std::string> diagnostics;
  std::chrono::steady_clock::time_point now{};
  DeviceService service(registry, [](Registration, cns::runtime::TimePoint) {
    return true;
  }, [](DesiredDeviceWrite) { return false; },
  [&](cns::runtime::PublishedState event) { events.push_back(std::move(event)); },
  [&] { return now; },
  [&](std::string error) { diagnostics.push_back(std::move(error)); });
  service.TryPush(Message(std::string{"cns/"} + kKnown + "/telemetry",
                          R"({"temperature":24})"));
  service.ProcessReady(cns::runtime::TimePoint{});
  now += 5s;
  service.ProcessReady(cns::runtime::TimePoint{});
  REQUIRE_FALSE(diagnostics.empty());
  REQUIRE_FALSE(events.empty());
  CHECK(events.back().degraded);
  CHECK(service.IsDeviceDegraded(kKnown));
}

TEST_CASE("business Run catches throwing clock port") {
  cns::device::DeviceRegistry registry;
  std::atomic_int calls{0};
  std::vector<std::string> diagnostics;
  std::mutex observed;
  DeviceService service(registry, [](Registration, cns::runtime::TimePoint) {
    return true;
  }, [](DesiredDeviceWrite) { return true; },
  [](cns::runtime::PublishedState) {},
  [&] {
    if (++calls > 1) throw std::runtime_error("boom");
    return std::chrono::steady_clock::time_point{};
  }, [&](std::string error) {
    std::lock_guard lock(observed); diagnostics.push_back(std::move(error));
  });
  std::jthread thread([&](std::stop_token stop) { service.Run(stop); });
  REQUIRE(WaitUntil([&] {
    std::lock_guard lock(observed); return !diagnostics.empty();
  }));
}

struct FakeStore : PostgresWorker::StorePort {
  std::atomic_bool connected{true};
  std::atomic_bool migration_ok{true};
  std::mutex calls_mutex;
  std::vector<std::string> calls;
  void Called(std::string call) {
    std::lock_guard lock(calls_mutex);
    calls.push_back(std::move(call));
  }
  std::vector<std::string> Calls() {
    std::lock_guard lock(calls_mutex);
    return calls;
  }
  std::expected<DeviceRecord, cns::runtime::DatabaseError> Provision(
      const Registration& registration, cns::runtime::TimePoint) override {
    Called("provision");
    if (!connected) return std::unexpected(cns::runtime::DatabaseError{
        cns::runtime::DatabaseError::Kind::kUnavailable, "数据库暂不可用"});
    return Record(registration.vendor_id);
  }
  std::expected<void, cns::runtime::DatabaseError> Write(const DesiredDeviceWrite&) override {
    Called("write");
    if (!connected) return std::unexpected(cns::runtime::DatabaseError{
        cns::runtime::DatabaseError::Kind::kUnavailable, "数据库暂不可用"});
    return {};
  }
  std::expected<void, cns::runtime::DatabaseError> ReconnectAndValidate() override {
    Called("validate");
    if (!connected) return std::unexpected(cns::runtime::DatabaseError{
        cns::runtime::DatabaseError::Kind::kUnavailable, "数据库暂不可用"});
    if (!migration_ok) return std::unexpected(cns::runtime::DatabaseError{
        cns::runtime::DatabaseError::Kind::kPermanent, "迁移版本不一致"});
    return {};
  }
};

bool WaitUntil(const std::function<bool()>& predicate,
               std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  return predicate();
}

TEST_CASE("postgres worker reconnects after five seconds then requests replay") {
  FakeStore store;
  store.connected = false;
  std::vector<DatabaseResult> results;
  std::atomic_int replay{0};
  std::atomic<std::int64_t> now_ms{0};
  std::mutex observed;
  PostgresWorker worker(store, [&](DatabaseResult r) {
    std::lock_guard lock(observed); results.push_back(std::move(r));
  }, [&] { ++replay; }, [&] {
    return std::chrono::steady_clock::time_point{std::chrono::milliseconds{now_ms.load()}};
  });
  std::jthread thread([&](std::stop_token stop) { worker.Run(stop); });
  REQUIRE(worker.SubmitWrite({Record(kKnown, 2), 2, false, true, false,
                              cns::persistence::Urgency::kImmediate}));
  REQUIRE(WaitUntil([&] { std::lock_guard lock(observed); return !results.empty(); }));
  { std::lock_guard lock(observed);
    REQUIRE(results.back().kind == DatabaseResult::Kind::kUnavailable); }
  store.connected = true;
  now_ms += 4999;
  std::this_thread::sleep_for(110ms);
  CHECK(replay == 0);
  now_ms += 1;
  REQUIRE(WaitUntil([&] { return replay == 1; }));
  REQUIRE(WaitUntil([&] { return store.Calls().back() == "write"; }));
  { std::lock_guard lock(observed);
    CHECK(results[results.size() - 2].kind == DatabaseResult::Kind::kRecovered); }
  CHECK(worker.FlushAndStop(500ms));
}

TEST_CASE("failed provision is discarded and recovery relies on retained replay") {
  FakeStore store;
  store.connected = false;
  std::vector<DatabaseResult> results;
  std::atomic_int replay{0};
  std::atomic<std::int64_t> now_ms{0};
  std::mutex observed;
  PostgresWorker worker(store, [&](DatabaseResult r) {
    std::lock_guard lock(observed); results.push_back(std::move(r));
  },
                        [&] { ++replay; }, [&] {
    return std::chrono::steady_clock::time_point{std::chrono::milliseconds{now_ms.load()}};
  });
  std::jthread thread([&](std::stop_token stop) { worker.Run(stop); });
  REQUIRE(worker.SubmitProvision({kNew, RegistrationStatus::kOnline, "SEU", "DCDW-001"}, {}));
  REQUIRE(WaitUntil([&] { std::lock_guard lock(observed); return !results.empty(); }));
  store.connected = true;
  now_ms += 5000;
  REQUIRE(WaitUntil([&] { return replay == 1; }));
  CHECK(replay == 1);
  CHECK(store.Calls() == std::vector<std::string>{"provision", "validate"});
}

TEST_CASE("postgres worker merges writes and closed submissions fail") {
  FakeStore store;
  std::vector<DatabaseResult> results;
  PostgresWorker worker(store, [&](DatabaseResult r) { results.push_back(std::move(r)); },
                        [] {}, [] { return std::chrono::steady_clock::time_point{}; });
  REQUIRE(worker.SubmitWrite({Record(kKnown, 2), 2, false, true, false,
                              cns::persistence::Urgency::kImmediate}));
  REQUIRE(worker.SubmitWrite({Record(kKnown, 3), 3, false, false, true,
                              cns::persistence::Urgency::kTelemetryBatch}));
  std::jthread thread([&](std::stop_token stop) { worker.Run(stop); });
  CHECK(worker.FlushAndStop(500ms));
  CHECK(store.Calls() == std::vector<std::string>{"write"});
  CHECK_FALSE(worker.SubmitProvision(
      {kNew, RegistrationStatus::kOnline, "SEU", "DCDW-001"}, {}));
}

TEST_CASE("flush only waits and times out while Run owns blocked store") {
  struct BlockingStore final : FakeStore {
    std::atomic_bool entered{false};
    std::atomic_bool release{false};
    std::expected<void, cns::runtime::DatabaseError> Write(
        const DesiredDeviceWrite&) override {
      entered = true;
      while (!release) std::this_thread::yield();
      return {};
    }
  } store;
  PostgresWorker worker(store, [](DatabaseResult) {}, [] {},
                        [] { return std::chrono::steady_clock::time_point{}; });
  std::jthread thread([&](std::stop_token stop) { worker.Run(stop); });
  REQUIRE(worker.SubmitWrite({Record(kKnown, 2), 2, false, true, false,
                              cns::persistence::Urgency::kImmediate}));
  REQUIRE(WaitUntil([&] { return store.entered.load(); }));
  const auto started = std::chrono::steady_clock::now();
  CHECK_FALSE(worker.FlushAndStop(20ms));
  CHECK(std::chrono::steady_clock::now() - started < 100ms);
  store.release = true;
  CHECK(worker.FlushAndStop(500ms));
}

TEST_CASE("permanent store error is reported without reconnect") {
  struct PermanentStore final : FakeStore {
    std::expected<void, cns::runtime::DatabaseError> Write(
        const DesiredDeviceWrite&) override {
      Called("write");
      return std::unexpected(cns::runtime::DatabaseError{
          cns::runtime::DatabaseError::Kind::kPermanent, "字段永久无效"});
    }
  } store;
  std::vector<DatabaseResult> results;
  std::mutex observed;
  PostgresWorker worker(store, [&](DatabaseResult result) {
    std::lock_guard lock(observed); results.push_back(std::move(result));
  }, [] {}, [] { return std::chrono::steady_clock::time_point{}; });
  std::jthread thread([&](std::stop_token stop) { worker.Run(stop); });
  REQUIRE(worker.SubmitWrite({Record(kKnown, 2), 2, false, true, false,
                              cns::persistence::Urgency::kImmediate}));
  REQUIRE(WaitUntil([&] { std::lock_guard lock(observed); return !results.empty(); }));
  { std::lock_guard lock(observed);
    CHECK(results.back().kind == DatabaseResult::Kind::kPermanentFailure); }
  CHECK(worker.FlushAndStop(500ms));
  CHECK(store.Calls() == std::vector<std::string>{"write"});
}

TEST_CASE("throwing store and callbacks do not terminate worker") {
  struct ThrowStore final : FakeStore {
    std::expected<void, cns::runtime::DatabaseError> Write(
        const DesiredDeviceWrite&) override {
      throw std::runtime_error("secret exception");
    }
  } store;
  std::vector<std::string> diagnostics;
  std::mutex observed;
  PostgresWorker worker(store, [](DatabaseResult) {
    throw std::runtime_error("result callback");
  }, [] { throw std::runtime_error("replay callback"); },
  [] { return std::chrono::steady_clock::time_point{}; },
  [&](std::string error) {
    std::lock_guard lock(observed); diagnostics.push_back(std::move(error));
  });
  std::jthread thread([&](std::stop_token stop) { worker.Run(stop); });
  REQUIRE(worker.SubmitWrite({Record(kKnown, 2), 2, false, true, false,
                              cns::persistence::Urgency::kImmediate}));
  REQUIRE(WaitUntil([&] {
    std::lock_guard lock(observed); return !diagnostics.empty();
  }));
  CHECK(worker.FlushAndStop(500ms));
}

TEST_CASE("worker permits only one provision in flight per vendor") {
  struct BlockingProvisionStore final : FakeStore {
    std::atomic_bool entered{false};
    std::atomic_bool release{false};
    std::expected<DeviceRecord, cns::runtime::DatabaseError> Provision(
        const Registration& registration, cns::runtime::TimePoint) override {
      Called("provision");
      entered = true;
      while (!release) std::this_thread::yield();
      return Record(registration.vendor_id);
    }
  } store;
  PostgresWorker worker(store, [](DatabaseResult) {}, [] {},
                        [] { return std::chrono::steady_clock::time_point{}; });
  std::jthread thread([&](std::stop_token stop) { worker.Run(stop); });
  REQUIRE(worker.SubmitProvision(
      {kNew, RegistrationStatus::kOnline, "SEU", "DCDW-001"}, {}));
  REQUIRE(WaitUntil([&] { return store.entered.load(); }));
  CHECK_FALSE(worker.SubmitProvision(
      {kNew, RegistrationStatus::kOnline, "SEU", "DCDW-002"}, {}));
  store.release = true;
  CHECK(worker.FlushAndStop(500ms));
  CHECK(std::ranges::count(store.Calls(), "provision") == 1);
}

TEST_CASE("provision submission is rejected while unavailable and accepted after recovery") {
  struct OnceUnavailableStore final : FakeStore {
    std::atomic_int writes{0};
    std::expected<void, cns::runtime::DatabaseError> Write(
        const DesiredDeviceWrite&) override {
      Called("write");
      if (++writes == 1) {
        return std::unexpected(cns::runtime::DatabaseError{
            cns::runtime::DatabaseError::Kind::kUnavailable,
            "连接详情不可泄露"});
      }
      return {};
    }
  } store;
  std::atomic<std::int64_t> now_ms{0};
  std::vector<DatabaseResult> results;
  std::mutex observed;
  PostgresWorker worker(store, [&](DatabaseResult result) {
    std::lock_guard lock(observed); results.push_back(std::move(result));
  }, [] {}, [&] {
    return std::chrono::steady_clock::time_point{std::chrono::milliseconds{now_ms.load()}};
  });
  std::jthread thread([&](std::stop_token stop) { worker.Run(stop); });
  REQUIRE(worker.SubmitWrite({Record(kKnown, 2), 2, false, true, false,
                              cns::persistence::Urgency::kImmediate}));
  REQUIRE(WaitUntil([&] {
    std::lock_guard lock(observed);
    return !results.empty() &&
           results.back().kind == DatabaseResult::Kind::kUnavailable;
  }));
  CHECK_FALSE(worker.SubmitProvision(
      {kNew, RegistrationStatus::kOnline, "SEU", "DCDW-001"}, {}));
  now_ms = 5000;
  REQUIRE(WaitUntil([&] {
    std::lock_guard lock(observed);
    return std::ranges::any_of(results, [](const DatabaseResult& result) {
      return result.kind == DatabaseResult::Kind::kRecovered;
    });
  }));
  REQUIRE(worker.SubmitProvision(
      {kNew, RegistrationStatus::kOnline, "SEU", "DCDW-001"}, {}));
  REQUIRE(WaitUntil([&] {
    const auto calls = store.Calls();
    return std::ranges::count(calls, "provision") == 1;
  }));
  CHECK(worker.FlushAndStop(500ms));
}

TEST_CASE("DeviceService alone merges latest registration while worker provisions once") {
  struct BlockingProvisionStore final : FakeStore {
    std::atomic_bool entered{false};
    std::atomic_bool release{false};
    std::expected<DeviceRecord, cns::runtime::DatabaseError> Provision(
        const Registration& registration, cns::runtime::TimePoint) override {
      Called("provision");
      entered = true;
      while (!release) std::this_thread::yield();
      return Record(registration.vendor_id);
    }
  } store;
  cns::device::DeviceRegistry registry;
  DeviceService* service_pointer = nullptr;
  PostgresWorker worker(store, [&](DatabaseResult result) {
    service_pointer->PushDatabaseResult(std::move(result));
  }, [] {}, [] { return std::chrono::steady_clock::time_point{}; });
  DeviceService service(registry,
      [&](Registration registration, cns::runtime::TimePoint at) {
        return worker.SubmitProvision(std::move(registration), at);
      }, [&](DesiredDeviceWrite write) {
        return worker.SubmitWrite(std::move(write));
      }, [](cns::runtime::PublishedState) {},
      [] { return std::chrono::steady_clock::time_point{}; });
  service_pointer = &service;
  std::jthread thread([&](std::stop_token stop) { worker.Run(stop); });
  service.TryPush(Message(std::string{"cns/"} + kNew + "/registration",
                          RegistrationJson(kNew, "SEU", "DCDW-001")));
  service.ProcessReady(cns::runtime::TimePoint{});
  REQUIRE(WaitUntil([&] { return store.entered.load(); }));
  service.TryPush(Message(std::string{"cns/"} + kNew + "/registration",
                          RegistrationJson(kNew, "SEU", "DCDW-002")));
  service.ProcessReady(cns::runtime::TimePoint{});
  store.release = true;
  REQUIRE(WaitUntil([&] { return store.Calls().size() == 1; }));
  REQUIRE(WaitUntil([&] {
    service.ProcessReady(cns::runtime::TimePoint{});
    return registry.Find(kNew) != nullptr;
  }));
  CHECK(std::ranges::count(store.Calls(), "provision") == 1);
  REQUIRE(registry.Find(kNew));
  CHECK(registry.Find(kNew)->dcdw_label == "DCDW-002");
  CHECK(worker.FlushAndStop(500ms));
  CHECK(store.Calls() == std::vector<std::string>{"provision", "write"});
}

TEST_CASE("shutdown handshake persists latest registration after inflight provision") {
  struct BlockingProvisionStore final : FakeStore {
    std::atomic_bool entered{false};
    std::atomic_bool release{false};
    std::expected<DeviceRecord, cns::runtime::DatabaseError> Provision(
        const Registration& registration, cns::runtime::TimePoint) override {
      Called("provision");
      entered = true;
      while (!release) std::this_thread::yield();
      return Record(registration.vendor_id);
    }
  } store;
  cns::device::DeviceRegistry registry;
  DeviceService* service_pointer = nullptr;
  PostgresWorker worker(store, [&](DatabaseResult result) {
    service_pointer->PushDatabaseResult(std::move(result));
  }, [] {}, [] { return std::chrono::steady_clock::time_point{}; });
  DeviceService service(
      registry,
      [&](Registration registration, cns::runtime::TimePoint at) {
        return worker.SubmitProvision(std::move(registration), at);
      },
      [&](DesiredDeviceWrite write) {
        return worker.SubmitWrite(std::move(write));
      },
      [](cns::runtime::PublishedState) {},
      [] { return std::chrono::steady_clock::time_point{}; });
  service_pointer = &service;
  std::jthread database_thread(
      [&](std::stop_token stop) { worker.Run(stop); });
  std::jthread business_thread(
      [&](std::stop_token stop) { service.Run(stop); });

  REQUIRE(service.TryPush(Message(std::string{"cns/"} + kNew + "/registration",
                                  RegistrationJson(kNew, "SEU", "DCDW-001"))));
  REQUIRE(WaitUntil([&] { return store.entered.load(); }));
  REQUIRE(service.TryPush(Message(std::string{"cns/"} + kNew + "/registration",
                                  RegistrationJson(kNew, "SEU", "DCDW-002"))));
  service.Close();
  REQUIRE(service.WaitForInputDrained(500ms));
  CHECK_FALSE(service.WaitForDatabaseIdle(20ms));

  store.release = true;
  REQUIRE(service.WaitForDatabaseIdle(500ms));
  CHECK(worker.FlushAndStop(5000ms));
  business_thread.join();
  database_thread.join();
  CHECK(store.Calls() == std::vector<std::string>{"provision", "write"});
  REQUIRE(registry.Find(kNew));
  CHECK(registry.Find(kNew)->dcdw_label == "DCDW-002");
}

TEST_CASE("permanent migration mismatch stops without replay or reconnect loop") {
  FakeStore store;
  store.connected = false;
  std::atomic<std::int64_t> now_ms{0};
  std::atomic_int replay{0};
  std::vector<DatabaseResult> results;
  std::mutex observed;
  PostgresWorker worker(store, [&](DatabaseResult result) {
    std::lock_guard lock(observed); results.push_back(std::move(result));
  }, [&] { ++replay; }, [&] {
    return std::chrono::steady_clock::time_point{std::chrono::milliseconds{now_ms.load()}};
  });
  std::jthread thread([&](std::stop_token stop) { worker.Run(stop); });
  REQUIRE(worker.SubmitWrite({Record(kKnown, 2), 2, false, true, false,
                              cns::persistence::Urgency::kImmediate}));
  REQUIRE(WaitUntil([&] { std::lock_guard lock(observed); return !results.empty(); }));
  store.connected = true;
  store.migration_ok = false;
  now_ms = 5000;
  REQUIRE(WaitUntil([&] {
    std::lock_guard lock(observed);
    return results.size() >= 2;
  }));
  { std::lock_guard lock(observed);
    CHECK(results.back().kind == DatabaseResult::Kind::kPermanentFailure); }
  CHECK(replay == 0);
  CHECK(worker.FlushAndStop(500ms));
  CHECK(store.Calls() == std::vector<std::string>{"write", "validate"});
}

TEST_CASE("throwing replay callback is diagnosed and worker still drains") {
  FakeStore store;
  store.connected = false;
  std::atomic<std::int64_t> now_ms{0};
  std::vector<std::string> diagnostics;
  std::mutex observed;
  PostgresWorker worker(store, [](DatabaseResult) {},
  [] { throw std::runtime_error("replay"); }, [&] {
    return std::chrono::steady_clock::time_point{std::chrono::milliseconds{now_ms.load()}};
  }, [&](std::string error) {
    std::lock_guard lock(observed); diagnostics.push_back(std::move(error));
  });
  std::jthread thread([&](std::stop_token stop) { worker.Run(stop); });
  REQUIRE(worker.SubmitWrite({Record(kKnown, 2), 2, false, true, false,
                              cns::persistence::Urgency::kImmediate}));
  REQUIRE(WaitUntil([&] {
    std::lock_guard lock(observed); return !diagnostics.empty();
  }));
  store.connected = true;
  now_ms = 5000;
  REQUIRE(WaitUntil([&] {
    std::lock_guard lock(observed);
    return std::ranges::any_of(diagnostics, [](const std::string& error) {
      return error.find("replay") != std::string::npos;
    });
  }));
  CHECK(worker.FlushAndStop(500ms));
}

TEST_CASE("write unavailable clears all accepted provisions before recovery") {
  struct BlockingUnavailableWrite final : FakeStore {
    std::atomic_bool entered{false};
    std::atomic_bool release{false};
    std::atomic_int attempts{0};
    std::expected<void, cns::runtime::DatabaseError> Write(
        const DesiredDeviceWrite&) override {
      Called("write");
      if (++attempts > 1) return {};
      entered = true;
      while (!release) std::this_thread::yield();
      return std::unexpected(cns::runtime::DatabaseError{
          cns::runtime::DatabaseError::Kind::kUnavailable,
          "password=do-not-log"});
    }
  } store;
  std::atomic<std::int64_t> now_ms{0};
  std::vector<DatabaseResult> results;
  std::vector<std::string> diagnostics;
  std::mutex observed;
  PostgresWorker worker(store, [&](DatabaseResult result) {
    std::lock_guard lock(observed); results.push_back(std::move(result));
  }, [] {}, [&] {
    return std::chrono::steady_clock::time_point{std::chrono::milliseconds{now_ms.load()}};
  }, [&](std::string diagnostic) {
    std::lock_guard lock(observed); diagnostics.push_back(std::move(diagnostic));
  });
  std::jthread thread([&](std::stop_token stop) { worker.Run(stop); });
  REQUIRE(worker.SubmitWrite({Record(kKnown, 2), 2, false, true, false,
                              cns::persistence::Urgency::kImmediate}));
  REQUIRE(WaitUntil([&] { return store.entered.load(); }));
  REQUIRE(worker.SubmitProvision(
      {kNew, RegistrationStatus::kOnline, "SEU", "DCDW-001"}, {}));
  REQUIRE(worker.SubmitProvision(
      {kOther, RegistrationStatus::kOnline, "SEU", "DCDW-002"}, {}));
  store.release = true;
  REQUIRE(WaitUntil([&] {
    std::lock_guard lock(observed); return !results.empty();
  }));
  store.connected = true;
  now_ms = 5000;
  REQUIRE(WaitUntil([&] { return store.Calls().size() >= 3; }));
  CHECK(store.Calls() == std::vector<std::string>{"write", "validate", "write"});
  { std::lock_guard lock(observed);
    CHECK(results.front().error.find("password") == std::string::npos);
    CHECK(std::ranges::none_of(diagnostics, [](const std::string& value) {
      return value.find("password") != std::string::npos;
    })); }
  CHECK(worker.FlushAndStop(500ms));
}

TEST_CASE("provision unavailable clears other vendors and merged latest candidate") {
  struct BlockingUnavailableProvision final : FakeStore {
    std::atomic_bool entered{false};
    std::atomic_bool release{false};
    std::expected<DeviceRecord, cns::runtime::DatabaseError> Provision(
        const Registration& registration, cns::runtime::TimePoint) override {
      Called("provision:" + registration.vendor_id);
      entered = true;
      while (!release) std::this_thread::yield();
      return std::unexpected(cns::runtime::DatabaseError{
          cns::runtime::DatabaseError::Kind::kUnavailable, "连接串秘密"});
    }
  } store;
  std::atomic<std::int64_t> now_ms{0};
  std::atomic_int results{0};
  PostgresWorker worker(store, [&](DatabaseResult) { ++results; }, [] {}, [&] {
    return std::chrono::steady_clock::time_point{std::chrono::milliseconds{now_ms.load()}};
  });
  std::jthread thread([&](std::stop_token stop) { worker.Run(stop); });
  REQUIRE(worker.SubmitProvision(
      {kNew, RegistrationStatus::kOnline, "SEU", "DCDW-001"}, {}));
  REQUIRE(WaitUntil([&] { return store.entered.load(); }));
  CHECK_FALSE(worker.SubmitProvision(
      {kNew, RegistrationStatus::kOnline, "SEU", "DCDW-009"}, {}));
  REQUIRE(worker.SubmitProvision(
      {kOther, RegistrationStatus::kOnline, "SEU", "DCDW-002"}, {}));
  store.release = true;
  REQUIRE(WaitUntil([&] { return results.load() == 1; }));
  store.connected = true;
  now_ms = 5000;
  REQUIRE(WaitUntil([&] { return store.Calls().size() >= 2; }));
  CHECK(store.Calls().size() == 2);
  CHECK(store.Calls().back() == "validate");
  CHECK(worker.FlushAndStop(500ms));
}
}  // namespace
