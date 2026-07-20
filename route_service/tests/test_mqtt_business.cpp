// 本文件验证 MQTT 设备业务消息的订阅、接收与状态事件发布语义。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <mosquitto.h>

#include "adapters/mqtt/mqtt_client.hpp"

#include <chrono>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

struct Injection {
  void* context = nullptr;
  void (*connect_callback)(struct mosquitto*, void*, int) = nullptr;
  void (*message_callback)(struct mosquitto*, void*, const struct mosquitto_message*) =
      nullptr;
  std::vector<std::tuple<std::string, int>> subscriptions;
  std::vector<std::string> unsubscriptions;
  std::vector<std::tuple<std::string, std::string, int, bool>> publications;
};

Injection* active = nullptr;

class ScopedInjection {
 public:
  explicit ScopedInjection(Injection& injection) {
    REQUIRE(active == nullptr);
    active = &injection;
  }
  ~ScopedInjection() { active = nullptr; }
};

cns::config::MqttConfig TestConfig() {
  return {.host = "mqtt.invalid",
          .port = 1883,
          .keepalive = 60s,
          .client_id = "mqtt-business-test",
          .username = "",
          .password = "",
          .reconnect_delay = 1s,
          .reconnect_delay_max = 4s};
}

std::unique_ptr<cns::mqtt::MqttClient> MakeClient(
    cns::logging::Logger& logger, Injection& injection) {
  static_cast<void>(injection);
  auto created = cns::mqtt::MqttClient::Create(TestConfig(), logger);
  REQUIRE(created.has_value());
  return std::move(*created);
}

}  // namespace

extern "C" struct mosquitto* __real_mosquitto_new(const char*, bool, void*);
extern "C" void __real_mosquitto_connect_callback_set(
    struct mosquitto*, void (*)(struct mosquitto*, void*, int));
extern "C" void __real_mosquitto_message_callback_set(
    struct mosquitto*,
    void (*)(struct mosquitto*, void*, const struct mosquitto_message*));

extern "C" struct mosquitto* __wrap_mosquitto_new(const char* id,
                                                     bool clean_session,
                                                     void* context) {
  active->context = context;
  return __real_mosquitto_new(id, clean_session, context);
}

extern "C" void __wrap_mosquitto_connect_callback_set(
    struct mosquitto*, void (*callback)(struct mosquitto*, void*, int)) {
  active->connect_callback = callback;
}

extern "C" void __wrap_mosquitto_message_callback_set(
    struct mosquitto*,
    void (*callback)(struct mosquitto*, void*, const struct mosquitto_message*)) {
  active->message_callback = callback;
}

extern "C" int __wrap_mosquitto_subscribe(struct mosquitto*, int*,
                                             const char* topic, int qos) {
  active->subscriptions.emplace_back(topic, qos);
  return MOSQ_ERR_SUCCESS;
}

extern "C" int __wrap_mosquitto_unsubscribe(struct mosquitto*, int*,
                                               const char* topic) {
  active->unsubscriptions.emplace_back(topic);
  return MOSQ_ERR_SUCCESS;
}

extern "C" int __wrap_mosquitto_publish(struct mosquitto*, int*,
                                           const char* topic, int payloadlen,
                                           const void* payload, int qos,
                                           bool retain) {
  active->publications.emplace_back(
      topic, std::string{static_cast<const char*>(payload),
                         static_cast<std::size_t>(payloadlen)},
      qos, retain);
  return MOSQ_ERR_SUCCESS;
}

TEST_CASE("连接成功后订阅registration QoS2和telemetry QoS0") {
  std::ostringstream out;
  std::ostringstream err;
  cns::logging::Logger logger(cns::logging::Level::kDebug, out, err);
  Injection injection;
  ScopedInjection scoped{injection};
  auto client = MakeClient(logger, injection);

  REQUIRE(client->SubscribeDeviceMessages("cns").has_value());
  REQUIRE(injection.connect_callback != nullptr);
  injection.connect_callback(nullptr, injection.context, 0);

  CHECK(injection.subscriptions ==
        std::vector<std::tuple<std::string, int>>{
            {"cns/+/registration", 2}, {"cns/+/telemetry", 0}});
}

TEST_CASE("消息回调在进入时记录时间并原样转交topic和payload") {
  std::ostringstream out;
  std::ostringstream err;
  cns::logging::Logger logger(cns::logging::Level::kDebug, out, err);
  Injection injection;
  ScopedInjection scoped{injection};
  auto client = MakeClient(logger, injection);
  cns::mqtt::InboundMessage received;
  std::chrono::system_clock::time_point handler_entered;
  REQUIRE(client->ConfigureBusinessMessages(
                    [&](cns::mqtt::InboundMessage message) {
                      handler_entered = std::chrono::system_clock::now();
                      received = std::move(message);
                    },
                    128)
              .has_value());
  REQUIRE(injection.message_callback != nullptr);
  std::string payload = R"({"unparsed":true})";
  mosquitto_message message{.mid = 1,
                            .topic = const_cast<char*>("cns/VENDOR12345678901234/telemetry"),
                            .payload = payload.data(),
                            .payloadlen = static_cast<int>(payload.size()),
                            .qos = 0,
                            .retain = false};

  injection.message_callback(nullptr, injection.context, &message);

  CHECK(received.topic == message.topic);
  CHECK(received.payload == payload);
  CHECK(received.received_at <= handler_entered);
}

TEST_CASE("超限payload在复制前拒绝且不调用handler") {
  std::ostringstream out;
  std::ostringstream err;
  cns::logging::Logger logger(cns::logging::Level::kDebug, out, err);
  Injection injection;
  ScopedInjection scoped{injection};
  auto client = MakeClient(logger, injection);
  int calls = 0;
  REQUIRE(client->ConfigureBusinessMessages(
                    [&](cns::mqtt::InboundMessage) { ++calls; }, 4)
              .has_value());
  mosquitto_message message{.mid = 1,
                            .topic = const_cast<char*>("cns/x/telemetry"),
                            .payload = reinterpret_cast<void*>(1),
                            .payloadlen = 5,
                            .qos = 0,
                            .retain = false};

  CHECK_NOTHROW(injection.message_callback(nullptr, injection.context, &message));
  CHECK(calls == 0);
  CHECK(err.str().find("MQTT消息payload超过大小限制") != std::string::npos);
}

TEST_CASE("handler异常不穿越C回调且中文错误限频") {
  std::ostringstream out;
  std::ostringstream err;
  cns::logging::Logger logger(cns::logging::Level::kDebug, out, err);
  Injection injection;
  ScopedInjection scoped{injection};
  auto client = MakeClient(logger, injection);
  REQUIRE(client->ConfigureBusinessMessages(
                    [](cns::mqtt::InboundMessage) { throw std::runtime_error("secret"); },
                    64)
              .has_value());
  std::string payload = "{}";
  mosquitto_message message{.mid = 1,
                            .topic = const_cast<char*>("cns/x/registration"),
                            .payload = payload.data(),
                            .payloadlen = 2,
                            .qos = 2,
                            .retain = true};

  CHECK_NOTHROW(injection.message_callback(nullptr, injection.context, &message));
  CHECK_NOTHROW(injection.message_callback(nullptr, injection.context, &message));
  CHECK(err.str().find("MQTT业务消息处理器异常") != std::string::npos);
  CHECK(err.str().find("secret") == std::string::npos);
  CHECK(err.str().find("MQTT业务消息处理器异常") ==
        err.str().rfind("MQTT业务消息处理器异常"));
}

TEST_CASE("状态事件使用QoS0且retain为false") {
  std::ostringstream out;
  std::ostringstream err;
  cns::logging::Logger logger(cns::logging::Level::kDebug, out, err);
  Injection injection;
  ScopedInjection scoped{injection};
  auto client = MakeClient(logger, injection);

  REQUIRE(client->PublishStateEvent("cns/events/devices/id/state", "{\"on\":true}")
              .has_value());
  CHECK(injection.publications ==
        std::vector<std::tuple<std::string, std::string, int, bool>>{
            {"cns/events/devices/id/state", "{\"on\":true}", 0, false}});
}

TEST_CASE("恢复registration retained消息先unsubscribe再以QoS2 subscribe") {
  std::ostringstream out;
  std::ostringstream err;
  cns::logging::Logger logger(cns::logging::Level::kDebug, out, err);
  Injection injection;
  ScopedInjection scoped{injection};
  auto client = MakeClient(logger, injection);

  REQUIRE(client->ReplayRetainedRegistrations("cns").has_value());
  CHECK(injection.unsubscriptions == std::vector<std::string>{"cns/+/registration"});
  CHECK(injection.subscriptions ==
        std::vector<std::tuple<std::string, int>>{{"cns/+/registration", 2}});
}
