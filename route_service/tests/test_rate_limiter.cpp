// 本文件验证限频窗口边界与 MQTT 客户端最小生命周期语义。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "adapters/mqtt/mqtt_client.hpp"
#include "core/runtime/rate_limiter.hpp"

#include <chrono>
#include <sstream>

using namespace std::chrono_literals;

TEST_CASE("同一键首次允许且窗口内拒绝并在到期点恢复") {
  cns::runtime::RateLimiter limiter(30s);
  const auto start = std::chrono::steady_clock::time_point{};

  CHECK(limiter.ShouldEmit("mqtt_disconnect", start));
  CHECK_FALSE(limiter.ShouldEmit("mqtt_disconnect", start + 29s));
  CHECK(limiter.ShouldEmit("mqtt_disconnect", start + 30s));
}

TEST_CASE("不同键的限频窗口互不影响") {
  cns::runtime::RateLimiter limiter(30s);
  const auto now = std::chrono::steady_clock::time_point{};

  CHECK(limiter.ShouldEmit("first", now));
  CHECK(limiter.ShouldEmit("second", now));
  CHECK_FALSE(limiter.ShouldEmit("first", now + 1s));
  CHECK_FALSE(limiter.ShouldEmit("second", now + 1s));
}

TEST_CASE("单调时间参数回退不会提前放行") {
  cns::runtime::RateLimiter limiter(30s);
  const auto start = std::chrono::steady_clock::time_point{} + 100s;

  CHECK(limiter.ShouldEmit("mqtt_disconnect", start));
  CHECK_FALSE(limiter.ShouldEmit("mqtt_disconnect", start - 1s));
  CHECK(limiter.ShouldEmit("mqtt_disconnect", start + 30s));
}

TEST_CASE("MQTT客户端停止幂等且异步首次网络失败不阻止启动") {
  std::ostringstream out;
  std::ostringstream err;
  cns::logging::Logger logger(cns::logging::Level::kDebug, out, err);
  const cns::config::MqttConfig config{
      .host = "127.0.0.1",
      .port = 1,
      .keepalive = 60s,
      .client_id = "cns-route-service-unit-test",
      .username = "",
      .password = "",
      .reconnect_delay = 1s,
      .reconnect_delay_max = 30s,
  };

  auto created = cns::mqtt::MqttClient::Create(config, logger);
  REQUIRE(created.has_value());
  CHECK_FALSE((*created)->IsConnected());
  const auto started = (*created)->Start();
  const std::string start_result =
      started.has_value() ? "启动成功" : started.error();
  INFO(start_result);
  REQUIRE(started.has_value());

  (*created)->Stop();
  (*created)->Stop();
  CHECK_FALSE((*created)->IsConnected());
}
