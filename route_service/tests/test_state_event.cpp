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
      .vendor_id = "A1b2C3d4E5f6G7h8I9j0", .revision = 42,
      .school_id = 7, .school_name = "SEU",
      .dcdw_label = std::nullopt, .model_version = "CNS v1.0",
      .online = false, .last_seen_at = std::nullopt,
      .telemetry_received_at = std::nullopt, .latest_telemetry = std::nullopt,
      .degraded = true};
  const auto at = std::chrono::sys_days{std::chrono::year{2026}/7/20} + 14h + 30min + 25s + 123ms;
  const auto event = cns::state_event::BuildStateEvent(
      snapshot, cns::state_event::ChangeReason::kActivityTimeout, at);
  CHECK(event == nlohmann::json{{"schema_version", 1}, {"event_type", "device_state"},
      {"event_at", "2026-07-20T14:30:25.123Z"}, {"revision", 42},
      {"vendor_id", snapshot.vendor_id}, {"school_name", "SEU"},
      {"dcdw_label", nullptr},
      {"model_version", "CNS v1.0"}, {"status", "offline"},
      {"last_seen_at", nullptr}, {"telemetry_received_at", nullptr},
      {"latest_telemetry", nullptr}, {"change_reason", "activity_timeout"},
      {"degraded", true}});
}

TEST_CASE("状态事件输出五种原因完整 telemetry 与 degraded 原值") {
  const auto at = std::chrono::sys_days{std::chrono::year{2026}/7/20};
  cns::state_event::Snapshot snapshot{
      .vendor_id = "A1b2C3d4E5f6G7h8I9j0", .revision = 9,
      .school_id = 7, .school_name = "SEU",
      .dcdw_label = "DCDW-001", .model_version = "CNS v1.0",
      .online = true, .last_seen_at = at,
      .telemetry_received_at = at,
      .latest_telemetry = nlohmann::json{
          {"unknown", nlohmann::json::array({1, 2})},
          {"alarms", nlohmann::json::array({{{"code", "0x0301"}}})}},
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
    CHECK(event["latest_telemetry"]["alarms"][0]["code"] == "0x0301");
    CHECK(event["revision"] == 9);
    CHECK_FALSE(event.contains("school_id"));
    CHECK(event["model_version"] == "CNS v1.0");
    CHECK(event["degraded"] == false);
    CHECK(event["last_seen_at"] == "2026-07-20T00:00:00.000Z");
  }
}

TEST_CASE("全量设备目录稳定排序并输出空数组") {
  const auto at = std::chrono::sys_days{std::chrono::year{2026}/7/27};
  const auto populated = cns::state_event::BuildDeviceDirectorySnapshot(
      {{"Z9y8X7w6V5u4T3s2R1q0", "SEU", "DCDW-002", "CNS v1.0", false},
       {"A1b2C3d4E5f6G7h8I9j0", "SEU", std::nullopt, "CNS v1.0", true}},
      3, at);
  CHECK(populated["schema_version"] == 1);
  CHECK(populated["event_type"] == "device_directory_snapshot");
  CHECK(populated["revision"] == 3);
  REQUIRE(populated["devices"].size() == 2);
  CHECK(populated["devices"][0] ==
        nlohmann::json{{"vendor_id", "A1b2C3d4E5f6G7h8I9j0"},
                       {"school_name", "SEU"},
                       {"dcdw_label", nullptr},
                       {"model_version", "CNS v1.0"},
                       {"status", "online"}});
  CHECK(populated["devices"][1]["vendor_id"] == "Z9y8X7w6V5u4T3s2R1q0");
  CHECK(populated["devices"][1]["status"] == "offline");
  CHECK_FALSE(populated["devices"][0].contains("school_id"));

  const auto empty =
      cns::state_event::BuildDeviceDirectorySnapshot({}, 4, at);
  CHECK(empty["devices"] == nlohmann::json::array());
}

TEST_CASE("全量目录只在稳定字段变化时递增版本且全量替换可重放") {
  cns::state_event::DeviceDirectory directory;
  const cns::state_event::DirectoryEntry online{
      "A1b2C3d4E5f6G7h8I9j0", "SEU", "DCDW-001", "CNS v1.0", true};
  const cns::state_event::DirectoryEntry offline{
      "Z9y8X7w6V5u4T3s2R1q0", "SEU", std::nullopt, "CNS v1.0", false};

  CHECK(directory.Replace({}));
  CHECK(directory.Revision() == 1);
  CHECK_FALSE(directory.Replace({}));
  CHECK(directory.Revision() == 1);

  CHECK(directory.Update(online));
  CHECK(directory.Revision() == 2);
  CHECK_FALSE(directory.Update(online));
  CHECK(directory.Revision() == 2);
  CHECK(directory.Update(offline));
  CHECK(directory.Revision() == 3);
  CHECK((directory.Entries() ==
         std::vector<cns::state_event::DirectoryEntry>{online, offline}));

  auto changed = online;
  changed.online = false;
  CHECK(directory.Update(changed));
  CHECK(directory.Revision() == 4);

  CHECK(directory.Replace({offline}));
  CHECK(directory.Revision() == 5);
  CHECK_FALSE(directory.Replace({offline}));
}
