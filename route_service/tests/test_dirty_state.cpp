#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "core/persistence/dirty_state.hpp"

using namespace std::chrono_literals;

namespace {

using cns::device::DeviceRecord;
using cns::device::Status;
using cns::persistence::DesiredDeviceWrite;
using cns::persistence::DirtyState;
using cns::persistence::Urgency;

DeviceRecord Record(std::string vendor, std::uint64_t revision) {
  return DeviceRecord{.vendor_id = std::move(vendor),
                      .school_id = 1,
                      .school_name = "SEU",
                      .dcdw_label = std::nullopt,
                      .model_version = "v1",
                      .status = Status::kOnline,
                      .last_seen_at = std::nullopt,
                      .latest_telemetry = nlohmann::json{{"seq", revision}},
                      .telemetry_received_at = std::nullopt,
                      .revision = revision};
}

DesiredDeviceWrite Write(std::string vendor, std::uint64_t revision,
                         bool metadata, bool status, bool telemetry,
                         Urgency urgency = Urgency::kTelemetryBatch) {
  return {.record = Record(std::move(vendor), revision),
          .revision = revision,
          .write_metadata = metadata,
          .write_status = status,
          .write_telemetry = telemetry,
          .urgency = urgency};
}

}  // namespace

TEST_CASE("同设备一百帧只保留最新期望状态") {
  DirtyState state;
  for (std::uint64_t revision = 1; revision <= 100; ++revision) {
    state.Mark(Write("vendor-a", revision, false, false, true));
  }
  CHECK(state.Size() == 1);
  const auto writes = state.TakeTelemetryDue(
      std::chrono::steady_clock::now() + 10s, 1s);
  REQUIRE(writes.size() == 1);
  CHECK(writes.front().revision == 100);
  CHECK(writes.front().record.latest_telemetry.value().at("seq") == 100);
}

TEST_CASE("不同设备独立保留") {
  DirtyState state;
  state.Mark(Write("vendor-a", 1, false, false, true));
  state.Mark(Write("vendor-b", 2, false, false, true));
  CHECK(state.Size() == 2);
  CHECK(state.TakeTelemetryDue(std::chrono::steady_clock::now() + 10s, 1s)
            .size() == 2);
}

TEST_CASE("立即任务优先并携带此前最新 telemetry") {
  DirtyState state;
  state.Mark(Write("vendor-a", 1, false, false, true));
  auto immediate = Write("vendor-a", 2, false, true, false,
                         Urgency::kImmediate);
  immediate.record.latest_telemetry = nlohmann::json{{"seq", 1}};
  state.Mark(std::move(immediate));

  const auto writes = state.TakeImmediate();
  REQUIRE(writes.size() == 1);
  CHECK(writes.front().write_status);
  CHECK(writes.front().write_telemetry);
  CHECK(writes.front().revision == 2);
  CHECK(writes.front().record.latest_telemetry.value().at("seq") == 1);
}

TEST_CASE("旧完成结果不清除期间到达的新 revision") {
  DirtyState state;
  state.Mark(Write("vendor-a", 1, false, true, false,
                   Urgency::kImmediate));
  REQUIRE(state.TakeImmediate().size() == 1);
  state.Mark(Write("vendor-a", 2, false, true, false,
                   Urgency::kImmediate));
  state.Complete("vendor-a", 1);
  const auto writes = state.TakeImmediate();
  REQUIRE(writes.size() == 1);
  CHECK(writes.front().revision == 2);
  CHECK(writes.front().write_status);
}

TEST_CASE("失败恢复与期间新值按字段合并") {
  DirtyState state;
  state.Mark(Write("vendor-a", 1, true, false, false,
                   Urgency::kImmediate));
  const auto failed = state.TakeImmediate();
  REQUIRE(failed.size() == 1);
  state.Mark(Write("vendor-a", 2, false, false, true));
  state.Restore(failed.front());

  const auto writes = state.TakeImmediate();
  REQUIRE(writes.size() == 1);
  CHECK(writes.front().revision == 2);
  CHECK(writes.front().write_metadata);
  CHECK(writes.front().write_telemetry);
  CHECK(writes.front().record.latest_telemetry.value().at("seq") == 2);
}

TEST_CASE("普通 telemetry 批次在间隔到期前不降级为立即任务") {
  DirtyState state;
  state.Mark(Write("vendor-a", 1, false, false, true));
  const auto now = std::chrono::steady_clock::now();
  CHECK(state.TakeImmediate().empty());
  CHECK(state.TakeTelemetryDue(now, 5s).empty());
  CHECK(state.TakeTelemetryDue(now + 4s, 5s).empty());
  const auto first_batch = state.TakeTelemetryDue(now + 5s, 5s);
  REQUIRE(first_batch.size() == 1);
  state.Mark(Write("vendor-a", 2, false, false, true));
  const auto next_batch_started = std::chrono::steady_clock::now();
  state.Complete("vendor-a", first_batch.front().revision);
  CHECK(state.TakeTelemetryDue(next_batch_started + 4s, 5s).empty());
  CHECK(state.TakeTelemetryDue(next_batch_started + 5s, 5s).size() == 1);
}
