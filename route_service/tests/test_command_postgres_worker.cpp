#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "core/runtime/postgres_worker.hpp"

#include <atomic>
#include <mutex>
#include <thread>

namespace {

using namespace std::chrono_literals;
using cns::runtime::CommandDatabaseResult;
using cns::runtime::CommandDatabaseTask;
using cns::runtime::CommandDatabaseValue;
using cns::runtime::DatabaseError;
using cns::runtime::PostgresWorker;

cns::command::CommandRecord Record() {
  const auto at = cns::command::TimePoint{};
  return {"550e8400-e29b-41d4-a716-446655440000",
          cns::command::CommandType::kConfig, "web-console", "req-1",
          std::nullopt, nlohmann::json::object(),
          cns::command::CommandStatus::kPending, std::nullopt, std::nullopt,
          std::nullopt, at, std::nullopt, at, std::nullopt};
}

struct FakeStore : PostgresWorker::StorePort {
  std::atomic_bool unavailable{false};
  std::mutex mutex;
  std::vector<std::thread::id> threads;
  void Called() {
    std::lock_guard lock(mutex);
    threads.push_back(std::this_thread::get_id());
  }
  std::vector<std::thread::id> Threads() {
    std::lock_guard lock(mutex);
    return threads;
  }
  std::expected<cns::device::DeviceRecord, DatabaseError> Provision(
      const cns::protocol::Registration&, cns::runtime::TimePoint) override {
    Called();
    return std::unexpected(DatabaseError{DatabaseError::Kind::kPermanent, "不用"});
  }
  std::expected<void, DatabaseError> Write(
      const cns::persistence::DesiredDeviceWrite&) override {
    Called();
    return {};
  }
  std::expected<void, DatabaseError> ReconnectAndValidate() override {
    Called();
    if (unavailable) return std::unexpected(
        DatabaseError{DatabaseError::Kind::kUnavailable, "暂不可用"});
    return {};
  }
  std::expected<std::optional<cns::command::CommandRecord>, DatabaseError>
  FindCommand(std::string_view, std::string_view) override {
    Called();
    if (unavailable) return std::unexpected(
        DatabaseError{DatabaseError::Kind::kUnavailable, "暂不可用"});
    return std::optional<cns::command::CommandRecord>{Record()};
  }
  std::expected<std::optional<cns::command::CommandRecord>, DatabaseError>
  FindCommandById(std::string_view) override {
    Called();
    if (unavailable) return std::unexpected(
        DatabaseError{DatabaseError::Kind::kUnavailable, "暂不可用"});
    return std::optional<cns::command::CommandRecord>{Record()};
  }
  std::expected<cns::command::CommandRecord, DatabaseError> InsertCommand(
      const cns::command::CommandRecord& command) override {
    Called();
    return command;
  }
  std::expected<cns::command::CommandRecord, DatabaseError> TransitionCommand(
      std::string_view, cns::command::CommandStatus,
      cns::command::CommandStatus, const cns::command::CommandUpdate&) override {
    Called();
    return Record();
  }
  std::expected<std::size_t, DatabaseError> CleanupCommands(
      cns::command::TimePoint, std::size_t batch_size) override {
    Called();
    return batch_size;
  }
  std::expected<std::vector<cns::command::CommandRecord>, DatabaseError>
  RecoverControlCommands(cns::command::TimePoint,
                         std::size_t limit) override {
    Called();
    if (unavailable) return std::unexpected(
        DatabaseError{DatabaseError::Kind::kUnavailable, "暂不可用"});
    auto record = Record();
    record.command_type = cns::command::CommandType::kControl;
    record.status = cns::command::CommandStatus::kDeliveryUncertain;
    return std::vector<cns::command::CommandRecord>(limit == 0 ? 0 : 1,
                                                    std::move(record));
  }
};

bool WaitUntil(const std::function<bool()>& predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 1s;
  while (!predicate() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  return predicate();
}

}  // namespace

TEST_CASE("命令队列按容量和operation_id去重且关闭后拒绝") {
  FakeStore store;
  PostgresWorker worker(store, [](cns::runtime::DatabaseResult) {}, [] {},
                        [] { return std::chrono::steady_clock::time_point{}; },
                        {}, 5s, [](CommandDatabaseResult) {}, 2);
  CHECK(worker.SubmitCommand(1, cns::runtime::FindCommandTask{"s", "r1"}));
  CHECK_FALSE(worker.SubmitCommand(1, cns::runtime::FindCommandTask{"s", "r1"}));
  CHECK(worker.SubmitCommand(2, cns::runtime::CleanupCommandsTask{{}, 1}));
  CHECK_FALSE(worker.SubmitCommand(3, cns::runtime::CleanupCommandsTask{{}, 1}));
  std::jthread thread([&](std::stop_token stop) { worker.Run(stop); });
  CHECK(worker.FlushAndStop(1s));
  CHECK_FALSE(worker.SubmitCommand(4, cns::runtime::CleanupCommandsTask{{}, 1}));
}

TEST_CASE("设备和命令数据库方法只由同一Run线程调用") {
  FakeStore store;
  std::vector<CommandDatabaseResult> results;
  PostgresWorker worker(store, [](cns::runtime::DatabaseResult) {}, [] {},
                        [] { return std::chrono::steady_clock::time_point{}; },
                        {}, 5s, [&](CommandDatabaseResult result) {
                          results.push_back(std::move(result));
                        }, 4);
  cns::persistence::DesiredDeviceWrite write{};
  write.record.vendor_id = "A1b2C3d4E5f6G7h8I9j0";
  REQUIRE(worker.SubmitWrite(std::move(write)));
  REQUIRE(worker.SubmitCommand(1, cns::runtime::InsertCommandTask{Record()}));
  std::jthread thread([&](std::stop_token stop) { worker.Run(stop); });
  CHECK(worker.FlushAndStop(1s));
  const auto threads = store.Threads();
  REQUIRE(threads.size() == 2);
  CHECK(threads[0] == threads[1]);
  REQUIRE(results.size() == 1);
  CHECK(results.front().operation_id == 1);
}

TEST_CASE("命令暂不可用时保留任务并在迁移校验恢复后完成") {
  FakeStore store;
  store.unavailable = true;
  std::atomic<std::int64_t> now_ms{0};
  std::vector<CommandDatabaseResult> results;
  std::mutex mutex;
  std::atomic_bool marked_unavailable{false};
  PostgresWorker worker(store, [](cns::runtime::DatabaseResult) {}, [] {}, [&] {
    return std::chrono::steady_clock::time_point{std::chrono::milliseconds{now_ms.load()}};
  }, [&](std::string message) {
    if (message == "数据库命令操作暂不可用") marked_unavailable = true;
  }, 5s, [&](CommandDatabaseResult result) {
    std::lock_guard lock(mutex);
    results.push_back(std::move(result));
  }, 4);
  REQUIRE(worker.SubmitCommand(7, cns::runtime::FindCommandTask{"s", "r"}));
  std::jthread thread([&](std::stop_token stop) { worker.Run(stop); });
  REQUIRE(WaitUntil([&] { return marked_unavailable.load(); }));
  store.unavailable = false;
  now_ms = 5000;
  REQUIRE(WaitUntil([&] {
    std::lock_guard lock(mutex);
    return results.size() == 1;
  }));
  CHECK(worker.FlushAndStop(1s));
  std::lock_guard lock(mutex);
  REQUIRE(results.front().value.has_value());
}

TEST_CASE("PostgresWorker执行按命令ID查询任务") {
  FakeStore store;
  std::vector<CommandDatabaseResult> results;
  PostgresWorker worker(store, [](cns::runtime::DatabaseResult) {}, [] {},
                        [] { return std::chrono::steady_clock::now(); }, {},
                        5s, [&](CommandDatabaseResult result) {
                          results.push_back(std::move(result));
                        });
  REQUIRE(worker.SubmitCommand(
      7, cns::runtime::FindCommandByIdTask{
             "550e8400-e29b-41d4-a716-446655440000"}));
  std::jthread thread([&](std::stop_token stop) { worker.Run(stop); });
  CHECK(worker.FlushAndStop(200ms));
  REQUIRE(results.size() == 1);
  const auto* value = std::get_if<std::optional<cns::command::CommandRecord>>(
      &*results.front().value);
  REQUIRE(value != nullptr);
  REQUIRE(value->has_value());
  CHECK((*value)->command_id == "550e8400-e29b-41d4-a716-446655440000");
}

TEST_CASE("命令永久错误只结束当前操作且不触发重连") {
  struct PermanentStore final : FakeStore {
    std::expected<std::optional<cns::command::CommandRecord>, DatabaseError>
    FindCommand(std::string_view, std::string_view) override {
      Called();
      return std::unexpected(
          DatabaseError{DatabaseError::Kind::kPermanent, "永久错误"});
    }
  } store;
  std::vector<CommandDatabaseResult> results;
  std::mutex results_mutex;
  PostgresWorker worker(store, [](cns::runtime::DatabaseResult) {}, [] {},
                        [] { return std::chrono::steady_clock::time_point{}; },
                        {}, 5s, [&](CommandDatabaseResult result) {
                          std::lock_guard lock(results_mutex);
                          results.push_back(std::move(result));
                        }, 2);
  REQUIRE(worker.SubmitCommand(1, cns::runtime::FindCommandTask{"s", "r"}));
  std::jthread thread([&](std::stop_token stop) { worker.Run(stop); });
  REQUIRE(WaitUntil([&] {
    std::lock_guard lock(results_mutex);
    return results.size() == 1;
  }));
  CHECK(worker.FlushAndStop(1s));
  std::lock_guard lock(results_mutex);
  REQUIRE_FALSE(results.front().value.has_value());
  CHECK(results.front().value.error().kind == DatabaseError::Kind::kPermanent);
  CHECK(store.Threads().size() == 1);
}

TEST_CASE("飞控恢复任务在工作线程返回完整收敛记录") {
  FakeStore store;
  std::vector<CommandDatabaseResult> results;
  PostgresWorker worker(store, [](cns::runtime::DatabaseResult) {}, [] {},
                        [] { return std::chrono::steady_clock::time_point{}; },
                        {}, 5s, [&](CommandDatabaseResult result) {
                          results.push_back(std::move(result));
                        }, 2);
  const auto recovered_at = cns::command::TimePoint{123s};
  REQUIRE(worker.SubmitCommand(
      8, cns::runtime::RecoverControlCommandsTask{recovered_at, 1}));
  std::jthread thread([&](std::stop_token stop) { worker.Run(stop); });
  CHECK(worker.FlushAndStop(1s));
  REQUIRE(results.size() == 1);
  REQUIRE(results.front().value.has_value());
  const auto* recovered = std::get_if<std::vector<cns::command::CommandRecord>>(
      &results.front().value.value());
  REQUIRE(recovered != nullptr);
  REQUIRE(recovered->size() == 1);
  CHECK(recovered->front().status ==
        cns::command::CommandStatus::kDeliveryUncertain);
}

TEST_CASE("停机等待命令任务且超时不越线程调用存储") {
  struct BlockingStore final : FakeStore {
    std::atomic_bool entered{false};
    std::atomic_bool release{false};
    std::expected<std::size_t, DatabaseError> CleanupCommands(
        cns::command::TimePoint, std::size_t batch_size) override {
      Called();
      entered = true;
      while (!release) std::this_thread::yield();
      return batch_size;
    }
  } store;
  PostgresWorker worker(store, [](cns::runtime::DatabaseResult) {}, [] {},
                        [] { return std::chrono::steady_clock::time_point{}; },
                        {}, 5s, [](CommandDatabaseResult) {}, 2);
  REQUIRE(worker.SubmitCommand(1, cns::runtime::CleanupCommandsTask{{}, 1}));
  std::jthread thread([&](std::stop_token stop) { worker.Run(stop); });
  REQUIRE(WaitUntil([&] { return store.entered.load(); }));
  CHECK_FALSE(worker.FlushAndStop(20ms));
  CHECK(store.Threads().size() == 1);
  store.release = true;
  CHECK(worker.FlushAndStop(1s));
}
