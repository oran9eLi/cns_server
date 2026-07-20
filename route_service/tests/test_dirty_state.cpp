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
  const auto old_write = state.TakeImmediate();
  REQUIRE(old_write.size() == 1);
  state.Mark(Write("vendor-a", 2, false, true, false,
                   Urgency::kImmediate));
  state.Complete(old_write.front());
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
  state.Complete(first_batch.front());
  CHECK(state.TakeTelemetryDue(next_batch_started + 4s, 5s).empty());
  CHECK(state.TakeTelemetryDue(next_batch_started + 5s, 5s).size() == 1);
}

TEST_CASE("新 telemetry 从非 dirty 变 dirty 时重新开始批次等待") {
  DirtyState state;
  const auto t0 = std::chrono::steady_clock::time_point{};
  state.Mark(Write("vendor-a", 1, false, true, false,
                   Urgency::kImmediate),
             t0);
  REQUIRE(state.TakeImmediate().size() == 1);

  state.Mark(Write("vendor-a", 2, false, false, true), t0 + 1h);
  CHECK(state.TakeTelemetryDue(t0 + 1h, 5s).empty());
  CHECK(state.TakeTelemetryDue(t0 + 1h + 4s, 5s).empty());
  CHECK(state.TakeTelemetryDue(t0 + 1h + 5s, 5s).size() == 1);
}

TEST_CASE("同 vendor 同 revision 的分开字段请求完成互不清理") {
  DirtyState state;
  const auto t0 = std::chrono::steady_clock::time_point{};
  state.Mark(Write("vendor-a", 7, false, false, true), t0);
  const auto telemetry = state.TakeTelemetryDue(t0 + 5s, 5s);
  REQUIRE(telemetry.size() == 1);

  state.Mark(Write("vendor-a", 7, false, true, false,
                   Urgency::kImmediate),
             t0 + 6s);
  const auto status = state.TakeImmediate();
  REQUIRE(status.size() == 1);
  state.Complete(status.front());
  state.Restore(telemetry.front());

  const auto retry = state.TakeTelemetryDue(t0 + 12s, 5s);
  REQUIRE(retry.size() == 1);
  CHECK(retry.front().write_telemetry);
  CHECK_FALSE(retry.front().write_status);
}

TEST_CASE("在途 telemetry 不因后来的立即 status 重复提交") {
  DirtyState state;
  const auto t0 = std::chrono::steady_clock::time_point{};
  state.Mark(Write("vendor-a", 1, false, false, true), t0);
  REQUIRE(state.TakeTelemetryDue(t0 + 5s, 5s).size() == 1);

  state.Mark(Write("vendor-a", 2, false, true, false,
                   Urgency::kImmediate),
             t0 + 6s);
  const auto status = state.TakeImmediate();
  REQUIRE(status.size() == 1);
  CHECK(status.front().write_status);
  CHECK_FALSE(status.front().write_telemetry);
}

TEST_CASE("排空时不等待批次间隔并提取每台设备最后 telemetry") {
  DirtyState state;
  const auto t0 = std::chrono::steady_clock::time_point{};
  state.Mark(Write("vendor-a", 1, false, false, true), t0);
  state.Mark(Write("vendor-a", 2, false, false, true), t0 + 1s);
  state.Mark(Write("vendor-b", 3, false, false, true), t0 + 1s);

  const auto writes = state.TakeAllDirty();
  REQUIRE(writes.size() == 2);
  const auto device_a = std::ranges::find_if(writes, [](const auto& write) {
    return write.record.vendor_id == "vendor-a";
  });
  REQUIRE(device_a != writes.end());
  CHECK(device_a->revision == 2);
  CHECK(device_a->record.latest_telemetry->at("seq") == 2);
  CHECK(state.TakeAllDirty().empty());
}
