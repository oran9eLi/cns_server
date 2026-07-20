#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "core/runtime/device_service.hpp"
#include "core/runtime/postgres_worker.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
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

struct Harness {
  cns::device::DeviceRegistry registry;
  std::vector<Registration> provisions;
  std::vector<DesiredDeviceWrite> writes;
  std::vector<cns::runtime::PublishedState> events;
  std::chrono::steady_clock::time_point steady{};

  DeviceService Make(std::size_t capacity = 8) {
    return DeviceService(
        registry,
        [this](Registration r, cns::runtime::TimePoint) {
          provisions.push_back(std::move(r));
        },
        [this](DesiredDeviceWrite w) { writes.push_back(std::move(w)); },
        [this](cns::runtime::PublishedState e) { events.push_back(std::move(e)); },
        [this] { return steady; }, capacity);
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
  service.ProcessReady();
  REQUIRE(h.provisions.size() == 1);
  CHECK(h.provisions.back().school_name == "SEU");
  CHECK(service.PendingRegistrationCount() == 1);
}

TEST_CASE("provision result installs device and applies latest registration") {
  Harness h;
  auto service = h.Make();
  service.TryPush(Message(std::string{"cns/"} + kNew + "/registration",
                          RegistrationJson(kNew, "SEU", "DCDW-002")));
  service.ProcessReady();
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

struct FakeStore final : PostgresWorker::StorePort {
  bool connected = true;
  bool migration_ok = true;
  std::vector<std::string> calls;
  std::expected<DeviceRecord, std::string> Provision(
      const Registration& registration, cns::runtime::TimePoint) override {
    calls.push_back("provision");
    if (!connected) return std::unexpected("down");
    return Record(registration.vendor_id);
  }
  std::expected<void, std::string> Write(const DesiredDeviceWrite&) override {
    calls.push_back("write");
    if (!connected) return std::unexpected("down");
    return {};
  }
  std::expected<void, std::string> ReconnectAndValidate() override {
    calls.push_back("validate");
    if (!connected || !migration_ok) return std::unexpected("down");
    return {};
  }
};

TEST_CASE("postgres worker reconnects after five seconds then requests replay") {
  FakeStore store;
  store.connected = false;
  std::vector<DatabaseResult> results;
  int replay = 0;
  std::chrono::steady_clock::time_point now{};
  PostgresWorker worker(store, [&](DatabaseResult r) { results.push_back(std::move(r)); },
                        [&] { ++replay; }, [&] { return now; });
  worker.SubmitWrite({Record(kKnown, 2), 2, false, true, false,
                      cns::persistence::Urgency::kImmediate});
  worker.ProcessReady();
  REQUIRE(results.back().kind == DatabaseResult::Kind::kUnavailable);
  store.connected = true;
  now += 4999ms;
  worker.ProcessReady();
  CHECK(replay == 0);
  now += 1ms;
  worker.ProcessReady();
  CHECK(store.calls.back() == "write");
  CHECK(replay == 1);
  CHECK(results[results.size() - 2].kind == DatabaseResult::Kind::kRecovered);
}

TEST_CASE("failed provision is discarded and recovery relies on retained replay") {
  FakeStore store;
  store.connected = false;
  std::vector<DatabaseResult> results;
  int replay = 0;
  std::chrono::steady_clock::time_point now{};
  PostgresWorker worker(store, [&](DatabaseResult r) { results.push_back(std::move(r)); },
                        [&] { ++replay; }, [&] { return now; });
  worker.SubmitProvision({kNew, RegistrationStatus::kOnline, "SEU", "DCDW-001"}, {});
  worker.ProcessReady();
  store.connected = true;
  now += 5s;
  worker.ProcessReady();
  CHECK(replay == 1);
  CHECK(store.calls == std::vector<std::string>{"provision", "validate"});
}

TEST_CASE("postgres worker drains and stop has a fixed upper bound") {
  FakeStore store;
  std::vector<DatabaseResult> results;
  PostgresWorker worker(store, [&](DatabaseResult r) { results.push_back(std::move(r)); },
                        [] {}, [] { return std::chrono::steady_clock::time_point{}; });
  worker.SubmitProvision({kNew, RegistrationStatus::kOnline, "SEU", "DCDW-001"}, {});
  CHECK(worker.FlushAndStop(5s));
  CHECK(store.calls == std::vector<std::string>{"provision"});
}
}  // namespace
