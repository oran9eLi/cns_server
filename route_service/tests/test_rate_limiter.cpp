// 本文件验证限频窗口边界与 MQTT 客户端最小生命周期语义。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <mosquitto.h>

#include "adapters/mqtt/mqtt_client.hpp"
#include "core/runtime/rate_limiter.hpp"

#include <chrono>
#include <sstream>
#include <string>
#include <vector>

using namespace std::chrono_literals;

namespace {

struct MosquittoInjection {
  int connect_result = MOSQ_ERR_SUCCESS;
  int loop_start_result = MOSQ_ERR_SUCCESS;
  int disconnect_result = MOSQ_ERR_SUCCESS;
  int loop_stop_result = MOSQ_ERR_SUCCESS;
  std::vector<std::string> calls;
};

MosquittoInjection* active_injection = nullptr;

class ScopedMosquittoInjection {
 public:
  explicit ScopedMosquittoInjection(MosquittoInjection& injection) {
    REQUIRE(active_injection == nullptr);
    active_injection = &injection;
  }

  ~ScopedMosquittoInjection() { active_injection = nullptr; }
};

cns::config::MqttConfig TestConfig() {
  return {
      .host = "mqtt.invalid",
      .port = 1883,
      .keepalive = 60s,
      .client_id = "cns-route-service-unit-test",
      .username = "",
      .password = "",
      .reconnect_delay = 1s,
      .reconnect_delay_max = 30s,
  };
}

}  // namespace

extern "C" int __real_mosquitto_connect_async(struct mosquitto*, const char*,
                                                int, int);
extern "C" int __real_mosquitto_loop_start(struct mosquitto*);
extern "C" int __real_mosquitto_disconnect(struct mosquitto*);
extern "C" int __real_mosquitto_loop_stop(struct mosquitto*, bool);

extern "C" int __wrap_mosquitto_connect_async(struct mosquitto* client,
                                                const char* host, int port,
                                                int keepalive) {
  if (active_injection == nullptr) {
    return __real_mosquitto_connect_async(client, host, port, keepalive);
  }
  active_injection->calls.emplace_back("connect_async");
  return active_injection->connect_result;
}

extern "C" int __wrap_mosquitto_loop_start(struct mosquitto* client) {
  if (active_injection == nullptr) return __real_mosquitto_loop_start(client);
  active_injection->calls.emplace_back("loop_start");
  return active_injection->loop_start_result;
}

extern "C" int __wrap_mosquitto_disconnect(struct mosquitto* client) {
  if (active_injection == nullptr) return __real_mosquitto_disconnect(client);
  active_injection->calls.emplace_back("disconnect");
  return active_injection->disconnect_result;
}

extern "C" int __wrap_mosquitto_loop_stop(struct mosquitto* client, bool force) {
  if (active_injection == nullptr) {
    return __real_mosquitto_loop_stop(client, force);
  }
  active_injection->calls.emplace_back(force ? "loop_stop(true)"
                                               : "loop_stop(false)");
  return active_injection->loop_stop_result;
}

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

TEST_CASE("MQTT同步ERRNO后仍启动网络循环并按顺序幂等停止") {
  std::ostringstream out;
  std::ostringstream err;
  cns::logging::Logger logger(cns::logging::Level::kDebug, out, err);
  MosquittoInjection injection{.connect_result = MOSQ_ERR_ERRNO, .calls = {}};
  ScopedMosquittoInjection scoped{injection};

  auto created = cns::mqtt::MqttClient::Create(TestConfig(), logger);
  REQUIRE(created.has_value());
  REQUIRE((*created)->Start().has_value());

  (*created)->Stop();
  (*created)->Stop();
  CHECK(injection.calls ==
        std::vector<std::string>{"connect_async", "loop_start", "disconnect",
                                 "loop_stop(false)"});
}

TEST_CASE("MQTT同步EAI失败时不启动网络循环") {
  std::ostringstream out;
  std::ostringstream err;
  cns::logging::Logger logger(cns::logging::Level::kDebug, out, err);
  MosquittoInjection injection{.connect_result = MOSQ_ERR_EAI, .calls = {}};
  ScopedMosquittoInjection scoped{injection};

  auto created = cns::mqtt::MqttClient::Create(TestConfig(), logger);
  REQUIRE(created.has_value());
  CHECK_FALSE((*created)->Start().has_value());
  CHECK(injection.calls == std::vector<std::string>{"connect_async"});
}

TEST_CASE("MQTT停止错误被记录且重复停止不重复调用库") {
  std::ostringstream out;
  std::ostringstream err;
  cns::logging::Logger logger(cns::logging::Level::kDebug, out, err);
  MosquittoInjection injection{
      .disconnect_result = MOSQ_ERR_INVAL,
      .loop_stop_result = MOSQ_ERR_INVAL,
      .calls = {},
  };
  ScopedMosquittoInjection scoped{injection};

  auto created = cns::mqtt::MqttClient::Create(TestConfig(), logger);
  REQUIRE(created.has_value());
  REQUIRE((*created)->Start().has_value());
  (*created)->Stop();
  (*created)->Stop();

  CHECK(injection.calls ==
        std::vector<std::string>{"connect_async", "loop_start", "disconnect",
                                 "loop_stop(false)"});
  CHECK(err.str().find("停止MQTT网络线程失败") != std::string::npos);
}
