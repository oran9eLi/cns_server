#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "core/protocol/device_message.hpp"

using cns::protocol::RegistrationStatus;

namespace {
constexpr auto kVendor = "A1b2C3d4E5f6G7h8I9j0";
}

TEST_CASE("解析完整与缺角色号的 online registration") {
  const auto complete = cns::protocol::ParseRegistration(
      R"({"schema_version":1,"vendor_id":"A1b2C3d4E5f6G7h8I9j0","school_name":"SEU","dcdw_label":"DCDW-001","status":"online","future":true})",
      kVendor);
  REQUIRE(complete);
  CHECK(complete->vendor_id == kVendor);
  CHECK(complete->status == RegistrationStatus::kOnline);
  CHECK(complete->school_name == "SEU");
  CHECK(complete->dcdw_label == "DCDW-001");

  const auto without_label = cns::protocol::ParseRegistration(
      R"({"schema_version":1,"vendor_id":"A1b2C3d4E5f6G7h8I9j0","school_name":"SEU","status":"online"})", kVendor);
  REQUIRE(without_label);
  CHECK_FALSE(without_label->dcdw_label);
}

TEST_CASE("解析最小 offline registration") {
  const auto result = cns::protocol::ParseRegistration(
      R"({"schema_version":1,"vendor_id":"A1b2C3d4E5f6G7h8I9j0","status":"offline"})", kVendor);
  REQUIRE(result);
  CHECK(result->status == RegistrationStatus::kOffline);
  CHECK_FALSE(result->school_name);
  CHECK_FALSE(result->dcdw_label);
}

TEST_CASE("拒绝 registration JSON 版本和身份错误") {
  for (const auto payload : {
           "not secret valid json",
           R"([])",
           R"({"schema_version":3,"vendor_id":"A1b2C3d4E5f6G7h8I9j0","status":"offline"})",
           R"({"schema_version":1,"vendor_id":"Z1b2C3d4E5f6G7h8I9j0","status":"offline"})"}) {
    const auto result = cns::protocol::ParseRegistration(payload, kVendor);
    CHECK_FALSE(result);
    CHECK(result.error().find(payload) == std::string::npos);
  }
}

TEST_CASE("registration 缺失 schema_version 时明确拒绝") {
  const auto result = cns::protocol::ParseRegistration(
      R"({"vendor_id":"A1b2C3d4E5f6G7h8I9j0","status":"offline"})",
      kVendor);
  REQUIRE_FALSE(result);
  CHECK(result.error() == "schema_version 必须是整数 1 或 2");
}

TEST_CASE("解析 PX4 schema v2 registration 与 telemetry") {
  constexpr auto device_id =
      "PX4U2-00112233445566778899AABBCCDDEEFF0011";
  const auto registration = cns::protocol::ParseRegistration(
      R"({"schema_version":2,"device_id":"PX4U2-00112233445566778899AABBCCDDEEFF0011","device_type":"flight_controller","status":"online","identity":{"uid2":"00112233445566778899AABBCCDDEEFF0011","remote_id":"1581F3411C32233939383438"}})",
      device_id);
  REQUIRE(registration);
  CHECK(registration->vendor_id == device_id);
  CHECK(registration->device_type ==
        cns::protocol::DeviceType::kFlightController);
  CHECK_FALSE(registration->school_name);

  const auto telemetry = cns::protocol::ParseTelemetry(
      R"({"schema_version":2,"device_id":"PX4U2-00112233445566778899AABBCCDDEEFF0011","device_type":"flight_controller","identity":{"remote_id":"1581F3411C32233939383438"},"telemetry":{"heartbeat":{"system_status":4}}})",
      device_id);
  REQUIRE(telemetry);
  CHECK(telemetry->device_type ==
        cns::protocol::DeviceType::kFlightController);
  CHECK(telemetry->payload["identity"]["remote_id"] ==
        "1581F3411C32233939383438");
}

TEST_CASE("拒绝 schema v2 消息体与 topic 设备 ID 不一致") {
  CHECK_FALSE(cns::protocol::ParseRegistration(
      R"({"schema_version":2,"device_id":"PX4U2-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA","device_type":"flight_controller","status":"online"})",
      "PX4U2-00112233445566778899AABBCCDDEEFF0011"));
  CHECK_FALSE(cns::protocol::ParseTelemetry(
      R"({"schema_version":2,"device_id":"PX4U1-0011223344556677","device_type":"flight_controller"})",
      "PX4U1-8899AABBCCDDEEFF"));
}

TEST_CASE("registration 缺失 vendor_id 时明确拒绝") {
  const auto result = cns::protocol::ParseRegistration(
      R"({"schema_version":1,"status":"offline"})", kVendor);
  REQUIRE_FALSE(result);
  CHECK(result.error() == "vendor_id 必须是非空字符串");
}

TEST_CASE("拒绝 registration 缺失空值与错误类型") {
  for (const auto payload : {
           R"({"schema_version":1,"vendor_id":"A1b2C3d4E5f6G7h8I9j0","status":"online"})",
           R"({"schema_version":1,"vendor_id":"A1b2C3d4E5f6G7h8I9j0","school_name":"","status":"online"})",
           R"({"schema_version":1,"vendor_id":"A1b2C3d4E5f6G7h8I9j0","school_name":7,"status":"online"})",
           R"({"schema_version":1,"vendor_id":"A1b2C3d4E5f6G7h8I9j0","school_name":"SEU","dcdw_label":"","status":"online"})",
           R"({"schema_version":1,"vendor_id":"A1b2C3d4E5f6G7h8I9j0","status":true})",
           R"({"schema_version":1,"vendor_id":"A1b2C3d4E5f6G7h8I9j0","status":"away"})"}) {
    CHECK_FALSE(cns::protocol::ParseRegistration(payload, kVendor));
  }
}

TEST_CASE("telemetry 保留完整对象并提取可选角色号") {
  const auto payload = R"({"identity":{"vendor_id":"A1b2C3d4E5f6G7h8I9j0","dcdw_label":"DCDW-001","future":3},"gps":{"lat":1},"unknown":[1,2]})";
  const auto result = cns::protocol::ParseTelemetry(payload, kVendor);
  REQUIRE(result);
  CHECK(result->dcdw_label == "DCDW-001");
  CHECK(result->payload == nlohmann::json::parse(payload));

  const auto no_identity = cns::protocol::ParseTelemetry(R"({"sensor":42})", kVendor);
  REQUIRE(no_identity);
  CHECK_FALSE(no_identity->dcdw_label);

  const auto schema_v1 = cns::protocol::ParseTelemetry(
      R"({"schema_version":1,"identity":{"vendor_id":"A1b2C3d4E5f6G7h8I9j0"},"sensor":43})",
      kVendor);
  REQUIRE(schema_v1);
  CHECK_FALSE(schema_v1->device_type);
}

TEST_CASE("拒绝非法 telemetry identity") {
  for (const auto payload : {
           R"([])", R"({"identity":7})",
           R"({"identity":{"vendor_id":7}})",
           R"({"identity":{"vendor_id":"Z1b2C3d4E5f6G7h8I9j0"}})",
           R"({"identity":{"dcdw_label":""}})"}) {
    CHECK_FALSE(cns::protocol::ParseTelemetry(payload, kVendor));
  }
}

TEST_CASE("telemetry 解析错误不回显原始 payload") {
  constexpr auto payload =
      R"({"identity":{"vendor_id":"不得出现在诊断中的敏感内容"}})";
  const auto result = cns::protocol::ParseTelemetry(payload, kVendor);
  REQUIRE_FALSE(result);
  CHECK(result.error().find(payload) == std::string::npos);
  CHECK(result.error().find("不得出现在诊断中的敏感内容") ==
        std::string::npos);
}
