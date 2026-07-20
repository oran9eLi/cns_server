#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "core/runtime/device_service.hpp"
#include "core/runtime/postgres_worker.hpp"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <future>
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
  std::vector<std::string> diagnostics;
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
        capacity);
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
  REQUIRE(h.registry.Load({Record(kKnown)}));
  auto service = h.Make();
  service.TryPush(Message(std::string{"cns/"} + kKnown + "/telemetry",
                          R"({"temperature":20})"));
  service.ProcessReady();
  CHECK(h.writes.empty());
  CHECK_FALSE(h.events.empty());
  CHECK_FALSE(h.events.back().degraded);
  h.steady += 5s;
  service.ProcessReady();
  REQUIRE(h.writes.size() == 1);
  CHECK(h.writes[0].write_telemetry);
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
  REQUIRE(worker.SubmitProvision(
      {kNew, RegistrationStatus::kOnline, "SEU", "DCDW-002"}, {}));
  store.release = true;
  CHECK(worker.FlushAndStop(500ms));
  CHECK(store.Calls() == std::vector<std::string>{"provision"});
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
}  // namespace
