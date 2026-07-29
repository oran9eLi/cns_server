#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "core/mqtt_topic/device_topic.hpp"

using cns::mqtt_topic::DeviceMessageKind;

TEST_CASE("只解析 namespace vendor kind 准确三段设备主题") {
  const auto registration = cns::mqtt_topic::ParseDeviceTopic("cns_rpi", "cns_rpi/A1b2C3d4E5f6G7h8I9j0/registration");
  REQUIRE(registration.has_value());
  CHECK(registration->vendor_id == "A1b2C3d4E5f6G7h8I9j0");
  CHECK(registration->kind == DeviceMessageKind::kRegistration);
  const auto telemetry = cns::mqtt_topic::ParseDeviceTopic("cns_rpi", "cns_rpi/A1b2C3d4E5f6G7h8I9j0/telemetry");
  REQUIRE(telemetry.has_value());
  CHECK(telemetry->kind == DeviceMessageKind::kTelemetry);
}

TEST_CASE("拒绝错误 namespace 额外层级和非法 vendor") {
  for (const auto topic : {"other/A1b2C3d4E5f6G7h8I9j0/registration",
                           "cns_rpi/A1b2C3d4E5f6G7h8I9j0/registration/extra",
                           "cns_rpi//registration",
                           "cns_rpi/device id/registration",
                           "cns_rpi/A1b2C3d4E5f6G7h8I9j0/unknown"}) {
    CHECK_FALSE(cns::mqtt_topic::ParseDeviceTopic("cns_rpi", topic).has_value());
  }
  CHECK(cns::mqtt_topic::IsValidVendorId("A1b2C3d4E5f6G7h8I9j0"));
  CHECK(cns::mqtt_topic::IsValidDeviceId(
      "PX4U2-00112233445566778899AABBCCDDEEFF0011"));
  CHECK_FALSE(cns::mqtt_topic::IsValidDeviceId("device id"));
}

TEST_CASE("构造订阅过滤器和状态事件主题") {
  CHECK(cns::mqtt_topic::RegistrationFilter("cns_rpi") == "cns_rpi/+/registration");
  CHECK(cns::mqtt_topic::TelemetryFilter("cns_rpi") == "cns_rpi/+/telemetry");
  CHECK(cns::mqtt_topic::DeviceDirectoryTopic("cns_rpi") ==
        "cns_rpi/events/devices/directory");
  CHECK(cns::mqtt_topic::StateEventTopic("cns_rpi", "A1b2C3d4E5f6G7h8I9j0") ==
        "cns_rpi/events/devices/A1b2C3d4E5f6G7h8I9j0/state");
}
