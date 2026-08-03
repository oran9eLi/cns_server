#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "core/command/command_router.hpp"

namespace {

using cns::command::CommandSource;
using cns::command::SourceCatalog;
using cns::command::SourceKind;
using cns::device::DeviceRecord;
using cns::device::DeviceRegistry;
using cns::device::Status;

constexpr auto kVendorA = "A1b2C3d4E5f6G7h8I9j0";
constexpr auto kVendorB = "Z9y8X7w6V5u4T3s2R1q0";
constexpr auto kVendorC = "M1n2B3v4C5x6Z7l8K9j0";

DeviceRecord Record(std::string device_id, std::int64_t school_id,
                    std::string school_name, std::string label,
                    Status status = Status::kOnline) {
  return {.device_id = std::move(device_id),
          .school_id = school_id,
          .school_name = std::move(school_name),
          .dcdw_label = std::move(label),
          .model_version = "v1",
          .status = status,
          .last_seen_at = std::nullopt,
          .latest_telemetry = std::nullopt,
          .telemetry_received_at = std::nullopt,
          .revision = 1};
}

cns::command::ConfigParameters Parameters() {
  cns::command::ConfigParameters parameters;
  parameters.heartbeat_interval_ms = 2000;
  return parameters;
}

cns::command::SourceConfigRequest DeviceLabelRequest(std::string label) {
  return {.request_id = "req-1",
          .target = cns::command::DeviceLabelTarget{std::move(label)},
          .parameters = Parameters(),
          .comparison_payload = nlohmann::json::object()};
}

cns::command::SourceConfigRequest DeviceIdRequest(std::string device_id) {
  return {.request_id = "req-1",
          .target = cns::command::DeviceTarget{std::move(device_id)},
          .parameters = Parameters(),
          .comparison_payload = nlohmann::json::object()};
}

cns::command::SourceConfigRequest LabelRequest(std::string school,
                                                std::string label) {
  return {.request_id = "req-1",
          .target = cns::command::SchoolLabelTarget{std::move(school),
                                                    std::move(label)},
          .parameters = Parameters(),
          .comparison_payload = nlohmann::json::object()};
}

}  // namespace

TEST_CASE("来源目录原子拒绝非法关联并保留禁用来源") {
  SourceCatalog catalog;
  REQUIRE(catalog.Load({{"web-console", SourceKind::kHostApp, std::nullopt, true}}));
  CHECK_FALSE(catalog.Load({{"duplicate", SourceKind::kHostApp, std::nullopt, true},
                            {"duplicate", SourceKind::kControlCenter, std::nullopt, true}}));
  REQUIRE(catalog.Find("web-console"));

  CHECK_FALSE(catalog.Load({{"device-a", SourceKind::kDevice, std::nullopt, true}}));
  CHECK_FALSE(catalog.Load({{"wrong-id", SourceKind::kDevice, kVendorA, true}}));
  CHECK_FALSE(catalog.Load({{"host", SourceKind::kHostApp, kVendorA, true}}));

  REQUIRE(catalog.Load({{kVendorA, SourceKind::kDevice, kVendorA, true},
                        {"disabled", SourceKind::kControlCenter, std::nullopt, false}}));
  REQUIRE(catalog.Find("disabled"));
  CHECK_FALSE(catalog.Find("disabled")->enabled);
  CHECK(catalog.Find("missing") == nullptr);
}

TEST_CASE("设备来源只能按自身学校角色寻址且不把离线作为路由错误") {
  DeviceRegistry devices;
  REQUIRE(devices.Load({Record(kVendorA, 1, "SEU", "DCDW-001"),
                        Record(kVendorB, 1, "SEU", "DCDW-002", Status::kOffline),
                        Record(kVendorC, 2, "Other", "DCDW-003")}));
  const CommandSource source{kVendorA, SourceKind::kDevice, kVendorA, true};

  auto self = cns::command::ResolveConfigTarget(source, DeviceLabelRequest("DCDW-001"), devices);
  REQUIRE(self);
  CHECK(self->device_id == kVendorA);

  auto same_school = cns::command::ResolveConfigTarget(
      source, DeviceLabelRequest("DCDW-002"), devices);
  REQUIRE(same_school);
  CHECK(same_school->device_id == kVendorB);

  auto other_school = cns::command::ResolveConfigTarget(
      source, DeviceLabelRequest("DCDW-003"), devices);
  REQUIRE_FALSE(other_school);
  CHECK(other_school.error().code == "target_not_found");

  auto wrong_form = cns::command::ResolveConfigTarget(source, DeviceIdRequest(kVendorB), devices);
  REQUIRE_FALSE(wrong_form);
  CHECK(wrong_form.error().code == "invalid_target");
}

TEST_CASE("上位机和管控中心支持两种寻址并稳定区分不存在与歧义") {
  DeviceRegistry devices;
  REQUIRE(devices.Load({Record(kVendorA, 1, "SEU", "DCDW-001"),
                        Record(kVendorB, 2, "SEU", "DCDW-001"),
                        Record(kVendorC, 3, "Other", "DCDW-003")}));
  const CommandSource source{"web-console", SourceKind::kHostApp, std::nullopt, true};

  auto device_id = cns::command::ResolveConfigTarget(source, DeviceIdRequest(kVendorC), devices);
  REQUIRE(device_id);
  CHECK(device_id->school_name == "Other");

  auto ambiguous = cns::command::ResolveConfigTarget(
      source, LabelRequest("SEU", "DCDW-001"), devices);
  REQUIRE_FALSE(ambiguous);
  CHECK(ambiguous.error().code == "target_ambiguous");

  auto missing = cns::command::ResolveConfigTarget(
      source, LabelRequest("Missing", "DCDW-001"), devices);
  REQUIRE_FALSE(missing);
  CHECK(missing.error().code == "target_not_found");
}

TEST_CASE("设备来源关联设备不存在时拒绝权限决策") {
  DeviceRegistry devices;
  REQUIRE(devices.Load({Record(kVendorB, 1, "SEU", "DCDW-002")}));
  const CommandSource source{kVendorA, SourceKind::kDevice, kVendorA, true};
  auto result = cns::command::ResolveConfigTarget(
      source, DeviceLabelRequest("DCDW-002"), devices);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == "permission_denied");
}

TEST_CASE("离线设备来源不能发起命令") {
  DeviceRegistry devices;
  REQUIRE(devices.Load({Record(kVendorA, 1, "SEU", "DCDW-001", Status::kOffline),
                        Record(kVendorB, 1, "SEU", "DCDW-002")}));
  const CommandSource source{kVendorA, SourceKind::kDevice, kVendorA, true};
  auto result = cns::command::ResolveConfigTarget(
      source, DeviceLabelRequest("DCDW-002"), devices);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == "source_device_offline");
}

TEST_CASE("真实 PX4 允许配置但拒绝主控箱私有控制命令") {
  auto px4 = Record("PX4RID123456789ABCDE",
                    0, "", "");
  px4.model_version = "PX4";
  px4.device_type = cns::protocol::DeviceType::kFlightController;
  px4.dcdw_label = std::nullopt;
  DeviceRegistry devices;
  REQUIRE(devices.Load({px4}));
  const CommandSource source{
      "web-console", SourceKind::kHostApp, std::nullopt, true};

  auto config = cns::command::ResolveConfigTarget(
      source, DeviceIdRequest(px4.device_id), devices);
  REQUIRE(config);

  const cns::command::SourceControlRequest control{
      .request_id = "req-px4",
      .target = cns::command::DeviceTarget{px4.device_id},
      .command = cns::command::ControlCommand::kTakeoff,
      .parameters = {},
      .comparison_payload = nlohmann::json::object(),
  };
  auto result = cns::command::ResolveControlTarget(source, control, devices);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == "unsupported_device_type");
}
