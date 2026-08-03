#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <vector>

#include "core/protocol/device_message.hpp"

using cns::protocol::DeviceType;
using cns::protocol::RegistrationStatus;

namespace {
constexpr auto kCnsDeviceId = "DCDWCNS1S2MEAG1VTA0C";
constexpr auto kPx4DeviceId = "PX4RID123456789ABCDE";
}

TEST_CASE("schema v3 online registration 接受统一身份与产品元数据") {
  const auto result = cns::protocol::ParseRegistration(
      R"({"schema_version":3,"device_id":"DCDWCNS1S2MEAG1VTA0C","device_type":"cns_box","status":"online","capabilities":["telemetry","remote_id","runtime_config"],"school_name":"SEU","dcdw_label":"DCDW-001","product":{"manufacturer_code":"DCDW","model_code":"CNS1"},"version":{"hardware":"1","firmware":"3.0.0"}})",
      kCnsDeviceId);

  REQUIRE(result);
  CHECK(result->device_id == kCnsDeviceId);
  CHECK(result->status == RegistrationStatus::kOnline);
  CHECK(result->device_type == DeviceType::kCnsBox);
  CHECK(result->school_name == "SEU");
  CHECK(result->dcdw_label == "DCDW-001");
  REQUIRE(result->capabilities);
  CHECK(*result->capabilities ==
        std::vector<std::string>{"telemetry", "remote_id", "runtime_config"});
  REQUIRE(result->product);
  CHECK((*result->product)["manufacturer_code"] == "DCDW");
  CHECK((*result->product)["model_code"] == "CNS1");
  REQUIRE(result->version);
  CHECK((*result->version)["hardware"] == "1");
  CHECK((*result->version)["firmware"] == "3.0.0");
}

TEST_CASE("schema v3 offline registration 只要求定位状态所需字段") {
  const auto result = cns::protocol::ParseRegistration(
      R"({"schema_version":3,"device_id":"PX4RID123456789ABCDE","device_type":"flight_controller","status":"offline"})",
      kPx4DeviceId);

  REQUIRE(result);
  CHECK(result->device_id == kPx4DeviceId);
  CHECK(result->status == RegistrationStatus::kOffline);
  CHECK(result->device_type == DeviceType::kFlightController);
  CHECK_FALSE(result->school_name);
  CHECK_FALSE(result->dcdw_label);
  CHECK_FALSE(result->capabilities);
  CHECK_FALSE(result->product);
  CHECK_FALSE(result->version);
}

TEST_CASE("registration 拒绝 schema v1 和 v2") {
  for (const auto payload : {
           R"({"schema_version":1,"device_id":"DCDWCNS1S2MEAG1VTA0C","school_name":"SEU","status":"online"})",
           R"({"schema_version":2,"device_id":"DCDWCNS1S2MEAG1VTA0C","device_type":"cns_box","status":"online","identity":{"school_name":"SEU"}})"}) {
    CHECK_FALSE(cns::protocol::ParseRegistration(payload, kCnsDeviceId));
  }
}

TEST_CASE("telemetry 拒绝 schema v1 和 v2") {
  for (const auto payload : {
           R"({"schema_version":1,"identity":{"device_id":"DCDWCNS1S2MEAG1VTA0C"},"telemetry":{}})",
           R"({"schema_version":2,"device_id":"DCDWCNS1S2MEAG1VTA0C","device_type":"cns_box","telemetry":{}})"}) {
    CHECK_FALSE(cns::protocol::ParseTelemetry(payload, kCnsDeviceId));
  }
}

TEST_CASE("schema v3 telemetry 保留完整慢速帧") {
  constexpr auto payload =
      R"({"schema_version":3,"device_id":"PX4RID123456789ABCDE","device_type":"flight_controller","sent_at":"2026-08-03T10:00:00Z","telemetry":{"heartbeat":{"system_status":4}},"drone_id":{"basic_id":{"id_type":1,"ua_type":2}}})";
  const auto result = cns::protocol::ParseTelemetry(payload, kPx4DeviceId);

  REQUIRE(result);
  CHECK(result->device_type == DeviceType::kFlightController);
  CHECK(result->payload == nlohmann::json::parse(payload));
  CHECK_FALSE(result->dcdw_label);
}

TEST_CASE("schema v3 消息拒绝 payload 与 topic 身份不一致") {
  CHECK_FALSE(cns::protocol::ParseRegistration(
      R"({"schema_version":3,"device_id":"PX4RID000000000000000","device_type":"flight_controller","status":"offline"})",
      kPx4DeviceId));
  CHECK_FALSE(cns::protocol::ParseTelemetry(
      R"({"schema_version":3,"device_id":"PX4RID000000000000000","device_type":"flight_controller","sent_at":"2026-08-03T10:00:00Z","telemetry":{},"drone_id":{"basic_id":{"id_type":1,"ua_type":2}}})",
      kPx4DeviceId));
}

TEST_CASE("schema v3 拒绝已删除的身份与网关字段") {
  for (const auto extra : {
           R"("device_id":"legacy")", R"("remote_id":"legacy")",
           R"("uid":"1")", R"("uid2":"2")",
           R"("gateway_id":"rpi")", R"("rpi_serial":"rpi")",
           R"("endpoint":{"sysid":1})", R"("identity":{})"}) {
    const auto payload = std::string{
                             R"({"schema_version":3,"device_id":"PX4RID123456789ABCDE","device_type":"flight_controller","status":"offline",)"} +
                         extra + "}";
    CHECK_FALSE(cns::protocol::ParseRegistration(payload, kPx4DeviceId));
  }
}

TEST_CASE("schema v3 telemetry 拒绝 basic_id 中重复 uas_id") {
  const auto result = cns::protocol::ParseTelemetry(
      R"({"schema_version":3,"device_id":"PX4RID123456789ABCDE","device_type":"flight_controller","sent_at":"2026-08-03T10:00:00Z","telemetry":{},"drone_id":{"basic_id":{"id_type":1,"ua_type":2,"uas_id":"PX4RID123456789ABCDE"}}})",
      kPx4DeviceId);
  CHECK_FALSE(result);
}

TEST_CASE("schema v3 对注册和遥测必需字段执行类型校验") {
  for (const auto payload : {
           R"({"schema_version":3,"device_type":"cns_box","status":"online","school_name":"SEU"})",
           R"({"schema_version":3,"device_id":"DCDWCNS1S2MEAG1VTA0C","device_type":"unknown","status":"online","school_name":"SEU"})",
           R"({"schema_version":3,"device_id":"DCDWCNS1S2MEAG1VTA0C","device_type":"cns_box","status":"away","school_name":"SEU"})",
           R"({"schema_version":3,"device_id":"DCDWCNS1S2MEAG1VTA0C","device_type":"cns_box","status":"online","capabilities":{}})"}) {
    CHECK_FALSE(cns::protocol::ParseRegistration(payload, kCnsDeviceId));
  }

  for (const auto payload : {
           R"({"schema_version":3,"device_id":"PX4RID123456789ABCDE","device_type":"flight_controller","telemetry":{},"drone_id":{"basic_id":{}}})",
           R"({"schema_version":3,"device_id":"PX4RID123456789ABCDE","device_type":"flight_controller","sent_at":"2026-08-03T10:00:00Z","drone_id":{"basic_id":{}}})",
           R"({"schema_version":3,"device_id":"PX4RID123456789ABCDE","device_type":"flight_controller","sent_at":"2026-08-03T10:00:00Z","telemetry":{},"drone_id":{}})"}) {
    CHECK_FALSE(cns::protocol::ParseTelemetry(payload, kPx4DeviceId));
  }
}

TEST_CASE("解析错误不回显原始 payload") {
  constexpr auto payload =
      R"({"schema_version":3,"device_id":"不得出现在诊断中的敏感内容","device_type":"flight_controller","status":"offline"})";
  const auto result = cns::protocol::ParseRegistration(payload, kPx4DeviceId);
  REQUIRE_FALSE(result);
  CHECK(result.error().find(payload) == std::string::npos);
  CHECK(result.error().find("不得出现在诊断中的敏感内容") ==
        std::string::npos);
}
