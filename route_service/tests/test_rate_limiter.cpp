// 本文件验证限频窗口边界与 MQTT 客户端最小生命周期语义。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <mosquitto.h>

#include "adapters/mqtt/mqtt_client.hpp"
#include "core/runtime/rate_limiter.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

struct MosquittoInjection {
  explicit MosquittoInjection(
      std::vector<int> results = {MOSQ_ERR_SUCCESS})
      : connect_results(std::move(results)) {}

  std::vector<int> connect_results{MOSQ_ERR_SUCCESS};
  int loop_start_result = MOSQ_ERR_SUCCESS;
  int disconnect_result = MOSQ_ERR_SUCCESS;
  int loop_stop_result = MOSQ_ERR_SUCCESS;
  void (*connect_callback)(struct mosquitto*, void*, int) = nullptr;
  void (*disconnect_callback)(struct mosquitto*, void*, int) = nullptr;
  void (*log_callback)(struct mosquitto*, void*, int, const char*) = nullptr;
  void* callback_context = nullptr;
  std::mutex mutex;
  std::condition_variable changed;
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
extern "C" void __real_mosquitto_destroy(struct mosquitto*);
extern "C" int __real_mosquitto_lib_cleanup();
extern "C" struct mosquitto* __real_mosquitto_new(const char*, bool, void*);
extern "C" void __real_mosquitto_connect_callback_set(
    struct mosquitto*, void (*)(struct mosquitto*, void*, int));
extern "C" void __real_mosquitto_disconnect_callback_set(
    struct mosquitto*, void (*)(struct mosquitto*, void*, int));
extern "C" void __real_mosquitto_log_callback_set(
    struct mosquitto*, void (*)(struct mosquitto*, void*, int, const char*));

extern "C" int __wrap_mosquitto_connect_async(struct mosquitto* client,
                                                const char* host, int port,
                                                int keepalive) {
  if (active_injection == nullptr) {
    return __real_mosquitto_connect_async(client, host, port, keepalive);
  }
  std::lock_guard lock{active_injection->mutex};
  active_injection->calls.emplace_back("connect_async");
  const std::size_t attempt = static_cast<std::size_t>(std::count(
      active_injection->calls.begin(), active_injection->calls.end(),
      "connect_async"));
  const std::size_t result_index =
      std::min(attempt - 1, active_injection->connect_results.size() - 1);
  const int result = active_injection->connect_results[result_index];
  active_injection->changed.notify_all();
  return result;
}

extern "C" int __wrap_mosquitto_loop_start(struct mosquitto* client) {
  if (active_injection == nullptr) return __real_mosquitto_loop_start(client);
  std::lock_guard lock{active_injection->mutex};
  active_injection->calls.emplace_back("loop_start");
  active_injection->changed.notify_all();
  return active_injection->loop_start_result;
}

extern "C" int __wrap_mosquitto_disconnect(struct mosquitto* client) {
  if (active_injection == nullptr) return __real_mosquitto_disconnect(client);
  std::lock_guard lock{active_injection->mutex};
  active_injection->calls.emplace_back("disconnect");
  return active_injection->disconnect_result;
}

extern "C" int __wrap_mosquitto_loop_stop(struct mosquitto* client, bool force) {
  if (active_injection == nullptr) {
    return __real_mosquitto_loop_stop(client, force);
  }
  std::lock_guard lock{active_injection->mutex};
  active_injection->calls.emplace_back(force ? "loop_stop(true)"
                                               : "loop_stop(false)");
  return active_injection->loop_stop_result;
}

extern "C" void __wrap_mosquitto_destroy(struct mosquitto* client) {
  if (active_injection == nullptr) {
    __real_mosquitto_destroy(client);
    return;
  }
  std::lock_guard lock{active_injection->mutex};
  active_injection->calls.emplace_back("destroy");
  __real_mosquitto_destroy(client);
}

extern "C" int __wrap_mosquitto_lib_cleanup() {
  if (active_injection == nullptr) return __real_mosquitto_lib_cleanup();
  std::lock_guard lock{active_injection->mutex};
  active_injection->calls.emplace_back("lib_cleanup");
  return __real_mosquitto_lib_cleanup();
}

extern "C" struct mosquitto* __wrap_mosquitto_new(const char* id,
                                                    bool clean_session,
                                                    void* context) {
  if (active_injection != nullptr) {
    active_injection->callback_context = context;
  }
  return __real_mosquitto_new(id, clean_session, context);
}

extern "C" void __wrap_mosquitto_connect_callback_set(
    struct mosquitto* client,
    void (*callback)(struct mosquitto*, void*, int)) {
  if (active_injection == nullptr) {
    __real_mosquitto_connect_callback_set(client, callback);
    return;
  }
  active_injection->connect_callback = callback;
}

extern "C" void __wrap_mosquitto_disconnect_callback_set(
    struct mosquitto* client,
    void (*callback)(struct mosquitto*, void*, int)) {
  if (active_injection == nullptr) {
    __real_mosquitto_disconnect_callback_set(client, callback);
    return;
  }
  active_injection->disconnect_callback = callback;
}

extern "C" void __wrap_mosquitto_log_callback_set(
    struct mosquitto* client,
    void (*callback)(struct mosquitto*, void*, int, const char*)) {
  if (active_injection == nullptr) {
    __real_mosquitto_log_callback_set(client, callback);
    return;
  }
  active_injection->log_callback = callback;
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
  MosquittoInjection injection{{MOSQ_ERR_ERRNO}};
  ScopedMosquittoInjection scoped{injection};

  auto created = cns::mqtt::MqttClient::Create(TestConfig(), logger);
  REQUIRE(created.has_value());
  REQUIRE((*created)->Start().has_value());

  (*created)->Stop();
  (*created)->Stop();
  created->reset();
  CHECK(std::ranges::equal(
      injection.calls,
      std::vector<std::string>{"connect_async", "loop_start", "disconnect",
                               "loop_stop(false)", "destroy", "lib_cleanup"}));
}

TEST_CASE("MQTT同步EAI多次后在后台恢复并启动网络循环") {
  std::ostringstream out;
  std::ostringstream err;
  cns::logging::Logger logger(cns::logging::Level::kDebug, out, err);
  MosquittoInjection injection{
      {MOSQ_ERR_EAI, MOSQ_ERR_EAI, MOSQ_ERR_SUCCESS}};
  ScopedMosquittoInjection scoped{injection};

  auto created = cns::mqtt::MqttClient::Create(TestConfig(), logger);
  REQUIRE(created.has_value());
  REQUIRE((*created)->Start().has_value());
  {
    std::unique_lock lock{injection.mutex};
    REQUIRE(injection.changed.wait_for(lock, 2s, [&] {
      return std::ranges::count(injection.calls, "loop_start") == 1;
    }));
  }
  (*created)->Stop();
  CHECK(std::ranges::count(injection.calls, "connect_async") == 3);
}

TEST_CASE("MQTT停止可中断EAI重试等待") {
  std::ostringstream out;
  std::ostringstream err;
  cns::logging::Logger logger(cns::logging::Level::kDebug, out, err);
  MosquittoInjection injection{{MOSQ_ERR_EAI}};
  ScopedMosquittoInjection scoped{injection};

  auto created = cns::mqtt::MqttClient::Create(TestConfig(), logger);
  REQUIRE(created.has_value());
  REQUIRE((*created)->Start().has_value());
  {
    std::unique_lock lock{injection.mutex};
    REQUIRE(injection.changed.wait_for(lock, 500ms, [&] {
      return std::ranges::count(injection.calls, "connect_async") >= 2;
    }));
  }
  const auto before = std::chrono::steady_clock::now();
  (*created)->Stop();
  CHECK(std::chrono::steady_clock::now() - before < 200ms);
  CHECK(std::ranges::count(injection.calls, "loop_start") == 0);
}

TEST_CASE("MQTT网络线程停止失败时析构不释放仍被引用的资源") {
  std::ostringstream out;
  std::ostringstream err;
  cns::logging::Logger logger(cns::logging::Level::kDebug, out, err);
  MosquittoInjection injection;
  injection.loop_stop_result = MOSQ_ERR_INVAL;
  ScopedMosquittoInjection scoped{injection};

  auto created = cns::mqtt::MqttClient::Create(TestConfig(), logger);
  REQUIRE(created.has_value());
  REQUIRE((*created)->Start().has_value());
  (*created)->Stop();
  created->reset();

  CHECK(std::ranges::count(injection.calls, "loop_stop(false)") == 2);
  CHECK(std::ranges::count(injection.calls, "destroy") == 0);
  CHECK(std::ranges::count(injection.calls, "lib_cleanup") == 0);
  CHECK(err.str().find("停止MQTT网络线程失败") != std::string::npos);
}

TEST_CASE("MQTT库错误日志按安全类别限频且不输出原始文本") {
  std::ostringstream out;
  std::ostringstream err;
  cns::logging::Logger logger(cns::logging::Level::kDebug, out, err);
  MosquittoInjection injection;
  ScopedMosquittoInjection scoped{injection};

  auto created = cns::mqtt::MqttClient::Create(TestConfig(), logger);
  REQUIRE(created.has_value());
  REQUIRE(injection.log_callback != nullptr);
  injection.log_callback(nullptr, injection.callback_context, MOSQ_LOG_ERR,
                         "password=绝不能输出");
  injection.log_callback(nullptr, injection.callback_context, MOSQ_LOG_ERR,
                         "password=绝不能输出");

  CHECK(err.str().find("MQTT库报告错误事件") != std::string::npos);
  CHECK(err.str().find("绝不能输出") == std::string::npos);
  CHECK(err.str().find("MQTT库报告错误事件") ==
        err.str().rfind("MQTT库报告错误事件"));
}
