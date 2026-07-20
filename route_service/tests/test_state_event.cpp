#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "core/state_event/state_event.hpp"

using namespace std::chrono_literals;

TEST_CASE("UTC 时间固定输出 RFC3339 毫秒") {
  const auto value = std::chrono::sys_days{std::chrono::year{2026}/7/20} + 14h + 30min + 25s + 123ms;
  CHECK(cns::state_event::FormatUtcRfc3339Millis(value) == "2026-07-20T14:30:25.123Z");
}

TEST_CASE("状态事件稳定输出完整字段和显式 null") {
  cns::state_event::Snapshot snapshot{
      .vendor_id = "A1b2C3d4E5f6G7h8I9j0", .school_name = "SEU",
      .dcdw_label = std::nullopt, .online = false, .last_seen_at = std::nullopt,
      .telemetry_received_at = std::nullopt, .latest_telemetry = std::nullopt,
      .degraded = true};
  const auto at = std::chrono::sys_days{std::chrono::year{2026}/7/20} + 14h + 30min + 25s + 123ms;
  const auto event = cns::state_event::BuildStateEvent(
      snapshot, cns::state_event::ChangeReason::kActivityTimeout, at);
  CHECK(event == nlohmann::json{{"schema_version", 1}, {"event_type", "device_state"},
      {"event_at", "2026-07-20T14:30:25.123Z"}, {"vendor_id", snapshot.vendor_id},
      {"school_name", "SEU"}, {"dcdw_label", nullptr}, {"status", "offline"},
      {"last_seen_at", nullptr}, {"telemetry_received_at", nullptr},
      {"latest_telemetry", nullptr}, {"change_reason", "activity_timeout"},
      {"degraded", true}});
}

TEST_CASE("状态事件输出五种原因完整 telemetry 与 degraded 原值") {
  const auto at = std::chrono::sys_days{std::chrono::year{2026}/7/20};
  cns::state_event::Snapshot snapshot{
      .vendor_id = "A1b2C3d4E5f6G7h8I9j0", .school_name = "SEU",
      .dcdw_label = "DCDW-001", .online = true, .last_seen_at = at,
      .telemetry_received_at = at, .latest_telemetry = nlohmann::json{{"unknown", nlohmann::json::array({1, 2})}},
      .degraded = false};
  const std::pair<cns::state_event::ChangeReason, const char*> cases[] = {
      {cns::state_event::ChangeReason::kRegistrationOnline, "registration_online"},
      {cns::state_event::ChangeReason::kRegistrationOffline, "registration_offline"},
      {cns::state_event::ChangeReason::kTelemetry, "telemetry"},
      {cns::state_event::ChangeReason::kActivityTimeout, "activity_timeout"},
      {cns::state_event::ChangeReason::kDatabaseRecovered, "database_recovered"}};
  for (const auto& [reason, text] : cases) {
    const auto event = cns::state_event::BuildStateEvent(snapshot, reason, at);
    CHECK(event["change_reason"] == text);
    CHECK(event["latest_telemetry"] == *snapshot.latest_telemetry);
    CHECK(event["degraded"] == false);
    CHECK(event["last_seen_at"] == "2026-07-20T00:00:00.000Z");
  }
}
