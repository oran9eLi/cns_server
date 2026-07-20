#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "core/runtime/device_ingress.hpp"

#include <chrono>
#include <string>
#include <vector>

using namespace std::chrono_literals;

TEST_CASE("MQTT队列满只输出安全计数和字节数且按窗口限频") {
  auto now = std::chrono::steady_clock::time_point{};
  std::vector<std::string> diagnostics;
  cns::runtime::DeviceIngress ingress(
      [](cns::mqtt::InboundMessage) { return false; },
      [&](std::string message) { diagnostics.push_back(std::move(message)); },
      [&] { return now; }, 30s);

  ingress.Handle({"cns/SECRET_VENDOR/telemetry", "secret", {}});
  ingress.Handle({"cns/OTHER/telemetry", "1234", {}});
  REQUIRE(diagnostics.size() == 1);
  CHECK(diagnostics.front() ==
        "MQTT设备消息队列已满，已拒绝消息，次数=1，字节数=6");
  CHECK(diagnostics.front().find("SECRET") == std::string::npos);
  CHECK(diagnostics.front().find("secret") == std::string::npos);

  now += 30s;
  ingress.Handle({"cns/THIRD/telemetry", "12", {}});
  REQUIRE(diagnostics.size() == 2);
  CHECK(diagnostics.back() ==
        "MQTT设备消息队列已满，已拒绝消息，次数=2，字节数=6");
}

TEST_CASE("诊断桥失效后队列拒绝不再调用外部端口") {
  int calls = 0;
  cns::runtime::DeviceIngress ingress(
      [](cns::mqtt::InboundMessage) { return false; },
      [&](std::string) { ++calls; });
  ingress.Disable();
  ingress.Handle({"cns/vendor/telemetry", "payload", {}});
  CHECK(calls == 0);
}
