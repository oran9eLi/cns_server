// 本文件验证来源配置请求、设备配置ACK与命令topic的严格协议。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "core/command/config_policy.hpp"
#include "core/command/source_request.hpp"

#include <cstdint>
#include <string>
#include <variant>

namespace {

using cns::command::DeviceConfigAck;
using cns::command::RejectedSourceRequest;
using cns::command::SourceConfigRequest;
using cns::command::SourceKind;

constexpr auto kVendor = "A1b2C3d4E5f6G7h8I9j0";
constexpr auto kCommand = "550e8400-e29b-41d4-a716-446655440000";

SourceConfigRequest ParseValid(std::string_view payload, SourceKind kind) {
  auto result = cns::command::ParseSourceConfigRequest(payload, kind);
  REQUIRE(std::holds_alternative<SourceConfigRequest>(result));
  return std::get<SourceConfigRequest>(std::move(result));
}

RejectedSourceRequest ParseRejected(std::string_view payload, SourceKind kind) {
  auto result = cns::command::ParseSourceConfigRequest(payload, kind);
  REQUIRE(std::holds_alternative<RejectedSourceRequest>(result));
  return std::get<RejectedSourceRequest>(std::move(result));
}

}  // namespace

TEST_CASE("命令topic必须准确匹配约定层级和标识") {
  const auto source = cns::command::ParseSourceRequestTopic(
      "cns_rpi", "cns_rpi/sources/web-console/config/request");
  REQUIRE(source.has_value());
  CHECK(*source == "web-console");

  const auto device = cns::command::ParseDeviceConfigAckTopic(
      "cns_rpi", std::string{"cns_rpi/"} + kVendor + "/config/ack");
  REQUIRE(device.has_value());
  CHECK(*device == kVendor);

  for (const auto topic : {"cns_rpi/sources/web-console/config",
                           "cns_rpi/sources/web/console/config/request",
                           "other/sources/web-console/config/request",
                           "cns_rpi/sources/web+console/config/request"}) {
    CHECK_FALSE(cns::command::ParseSourceRequestTopic("cns_rpi", topic).has_value());
  }
  const auto px4_device = cns::command::ParseDeviceConfigAckTopic(
      "cns_rpi", "cns_rpi/PX4RID123456789ABCDE/config/ack");
  REQUIRE(px4_device.has_value());
  CHECK(*px4_device == "PX4RID123456789ABCDE");

  CHECK_FALSE(cns::command::ParseDeviceConfigAckTopic(
                  "cns_rpi", "cns_rpi/bad+id/config/ack")
                  .has_value());
}

TEST_CASE("配置topic解析器拒绝飞控topic") {
  CHECK_FALSE(cns::command::ParseSourceRequestTopic(
                  "cns_rpi", "cns_rpi/sources/web-console/control/request")
                  .has_value());
  CHECK_FALSE(cns::command::ParseDeviceConfigAckTopic(
                  "cns_rpi", std::string{"cns_rpi/"} + kVendor +
                                 "/control/ack")
                  .has_value());
}

TEST_CASE("设备来源只接受同校角色号目标") {
  const auto request = ParseValid(
      R"({"schema_version":1,"request_id":"req-001","target":{"dcdw_label":"DCDW-002"},"parameters":{"heartbeat_interval_ms":2000}})",
      SourceKind::kDevice);
  CHECK(request.request_id == "req-001");
  REQUIRE(std::holds_alternative<cns::command::DeviceLabelTarget>(request.target));
  CHECK(std::get<cns::command::DeviceLabelTarget>(request.target).dcdw_label ==
        "DCDW-002");
  CHECK(request.parameters.heartbeat_interval_ms == 2000);
  CHECK(request.comparison_payload["schema_version"] == 1);

  const auto rejected = ParseRejected(
      std::string{"{\"schema_version\":1,\"request_id\":\"req-002\","
                  "\"target\":{\"device_id\":\""} + kVendor +
          "\"},\"parameters\":{\"heartbeat_interval_ms\":2000}}",
      SourceKind::kDevice);
  CHECK(rejected.request_id == "req-002");
  REQUIRE(rejected.comparison_payload.has_value());
  CHECK(rejected.error.code == "invalid_target");
}

TEST_CASE("非设备来源接受且只接受一种目标形式") {
  const auto by_device = ParseValid(
      std::string{"{\"schema_version\":1,\"request_id\":\"req-v\","
                  "\"target\":{\"device_id\":\""} + kVendor +
          "\"},\"parameters\":{\"mqtt_reconnect_delay_max_s\":60}}",
      SourceKind::kHostApp);
  CHECK(std::holds_alternative<cns::command::DeviceTarget>(by_device.target));

  const auto by_label = ParseValid(
      R"({"schema_version":1,"request_id":"req-l","target":{"school_name":"SEU","dcdw_label":"DCDW-002"},"parameters":{"telemetry_publish_interval_ms":100}})",
      SourceKind::kControlCenter);
  CHECK(std::holds_alternative<cns::command::SchoolLabelTarget>(by_label.target));

  const auto mixed = ParseRejected(
      std::string{"{\"schema_version\":1,\"request_id\":\"req-m\","
                  "\"target\":{\"device_id\":\""} + kVendor +
          "\",\"school_name\":\"SEU\",\"dcdw_label\":\"DCDW-002\"},"
          "\"parameters\":{\"heartbeat_interval_ms\":2000}}",
      SourceKind::kHostApp);
  CHECK(mixed.error.code == "invalid_target");
}

TEST_CASE("无效请求保留可用幂等上下文且不回显payload") {
  const auto wrong_schema = ParseRejected(
      R"({"schema_version":2,"request_id":"req-schema","target":{"dcdw_label":"DCDW-002"},"parameters":{"heartbeat_interval_ms":2000}})",
      SourceKind::kDevice);
  CHECK(wrong_schema.request_id == "req-schema");
  CHECK(wrong_schema.comparison_payload.has_value());
  CHECK(wrong_schema.error.code == "unsupported_schema_version");
  CHECK(wrong_schema.error.message.find("DCDW-002") == std::string::npos);

  const auto invalid_json = ParseRejected("{secret-payload", SourceKind::kDevice);
  CHECK_FALSE(invalid_json.request_id.has_value());
  CHECK_FALSE(invalid_json.comparison_payload.has_value());
  CHECK(invalid_json.error.code == "invalid_json");
  CHECK(invalid_json.error.message.find("secret-payload") == std::string::npos);

  const auto unknown = ParseRejected(
      R"({"schema_version":1,"request_id":"req-u","target":{"dcdw_label":"DCDW-002"},"parameters":{"heartbeat_interval_ms":2000},"secret":"never-log"})",
      SourceKind::kDevice);
  CHECK(unknown.error.code == "invalid_json");
  CHECK(unknown.error.message.find("never-log") == std::string::npos);

  CHECK_NOTHROW(ParseRejected(
      R"({"schema_version":18446744073709551615,"request_id":"req-big","target":{"dcdw_label":"DCDW-002"},"parameters":{"heartbeat_interval_ms":2000}})",
      SourceKind::kDevice));
}

TEST_CASE("四项配置参数接受端点并生成设备白名单payload") {
  const auto request = ParseValid(
      R"({"schema_version":1,"request_id":"req-all","target":{"dcdw_label":"DCDW-002"},"parameters":{"telemetry_publish_interval_ms":100,"heartbeat_interval_ms":60000,"mqtt_reconnect_delay_s":1,"mqtt_reconnect_delay_max_s":3600}})",
      SourceKind::kDevice);
  CHECK(request.parameters.telemetry_publish_interval_ms == 100);
  CHECK(request.parameters.heartbeat_interval_ms == 60000);
  CHECK(request.parameters.mqtt_reconnect_delay_s == 1);
  CHECK(request.parameters.mqtt_reconnect_delay_max_s == 3600);

  const auto payload = cns::command::BuildDeviceConfigSet(kCommand, request.parameters);
  CHECK(payload == nlohmann::json{
                       {"command_id", kCommand},
                       {"parameters",
                        {{"telemetry_publish_interval_ms", 100},
                         {"heartbeat_interval_ms", 60000},
                         {"mqtt_reconnect_delay_s", 1},
                         {"mqtt_reconnect_delay_max_s", 3600}}}});
}

TEST_CASE("配置参数拒绝空值未知字段类型范围和组合错误") {
  for (const auto payload : {
           R"({"schema_version":1,"request_id":"r1","target":{"dcdw_label":"DCDW-002"},"parameters":{}})",
           R"({"schema_version":1,"request_id":"r2","target":{"dcdw_label":"DCDW-002"},"parameters":{"unknown":1}})",
           R"({"schema_version":1,"request_id":"r3","target":{"dcdw_label":"DCDW-002"},"parameters":{"heartbeat_interval_ms":true}})",
           R"({"schema_version":1,"request_id":"r4","target":{"dcdw_label":"DCDW-002"},"parameters":{"heartbeat_interval_ms":100.5}})",
           R"({"schema_version":1,"request_id":"r5","target":{"dcdw_label":"DCDW-002"},"parameters":{"telemetry_publish_interval_ms":99}})",
           R"({"schema_version":1,"request_id":"r6","target":{"dcdw_label":"DCDW-002"},"parameters":{"heartbeat_interval_ms":60001}})",
           R"({"schema_version":1,"request_id":"r7","target":{"dcdw_label":"DCDW-002"},"parameters":{"mqtt_reconnect_delay_s":0}})",
           R"({"schema_version":1,"request_id":"r8","target":{"dcdw_label":"DCDW-002"},"parameters":{"mqtt_reconnect_delay_max_s":3601}})",
           R"({"schema_version":1,"request_id":"r9","target":{"dcdw_label":"DCDW-002"},"parameters":{"mqtt_reconnect_delay_s":31,"mqtt_reconnect_delay_max_s":30}})"}) {
    const auto rejected = ParseRejected(payload, SourceKind::kDevice);
    CHECK(rejected.error.code == "invalid_parameters");
  }
}

TEST_CASE("设备配置ACK严格校验并保留未来字段") {
  const auto applied = cns::command::ParseDeviceConfigAck(
      std::string{"{\"command_id\":\""} + kCommand +
      "\",\"status\":\"applied\",\"restart_required\":true,\"future\":3}");
  REQUIRE(applied.has_value());
  CHECK(applied->business_status == "applied");
  CHECK(applied->restart_required);
  CHECK(applied->raw["future"] == 3);

  const auto repeated = cns::command::ParseDeviceConfigAck(
      std::string{"{\"command_id\":\""} + kCommand +
      "\",\"status\":\"already_applied\",\"restart_required\":false}");
  REQUIRE(repeated.has_value());
  CHECK(repeated->business_status == "already_applied");

  const auto rejected = cns::command::ParseDeviceConfigAck(
      std::string{"{\"command_id\":\""} + kCommand +
      "\",\"status\":\"rejected\",\"error_code\":\"invalid_parameter\","
      "\"message\":\"参数非法\",\"restart_required\":false}");
  REQUIRE(rejected.has_value());
  CHECK(rejected->error_code == "invalid_parameter");

  CHECK_FALSE(cns::command::ParseDeviceConfigAck(
                  R"({"command_id":"not-uuid","status":"applied","restart_required":true})")
                  .has_value());
  CHECK_FALSE(cns::command::ParseDeviceConfigAck(
                  std::string{"{\"command_id\":\""} + kCommand +
                  "\",\"status\":\"rejected\",\"restart_required\":false}")
                  .has_value());
  CHECK_FALSE(cns::command::ParseDeviceConfigAck(
                  std::string{"{\"command_id\":\""} + kCommand +
                  "\",\"status\":\"rejected\",\"error_code\":\"Bad-Code\","
                  "\"restart_required\":false}")
                  .has_value());
}
