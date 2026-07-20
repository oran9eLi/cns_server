// 本文件验证命令消息的订阅、QoS 2 发布和发布完成关联。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <mosquitto.h>

#include <chrono>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "adapters/mqtt/mqtt_client.hpp"

using namespace std::chrono_literals;

namespace {

struct Injection {
  void* context = nullptr;
  void (*connect_callback)(struct mosquitto*, void*, int) = nullptr;
  void (*disconnect_callback)(struct mosquitto*, void*, int) = nullptr;
  void (*publish_callback)(struct mosquitto*, void*, int) = nullptr;
  std::vector<std::tuple<std::string, int>> subscriptions;
  std::vector<std::tuple<int, std::string, std::string, int, bool>> publications;
  int next_mid = 10;
};

Injection* active = nullptr;

class ScopedInjection {
 public:
  explicit ScopedInjection(Injection& injection) { active = &injection; }
  ~ScopedInjection() { active = nullptr; }
};

cns::config::MqttConfig TestConfig() {
  return {.host = "mqtt.invalid",
          .port = 1883,
          .keepalive = 60s,
          .client_id = "mqtt-command-test",
          .username = "",
          .password = "",
          .reconnect_delay = 1s,
          .reconnect_delay_max = 4s};
}

std::unique_ptr<cns::mqtt::MqttClient> MakeClient(
    cns::logging::Logger& logger) {
  auto created = cns::mqtt::MqttClient::Create(TestConfig(), logger);
  REQUIRE(created.has_value());
  return std::move(*created);
}

}  // namespace

extern "C" struct mosquitto* __real_mosquitto_new(const char*, bool, void*);

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

extern "C" void __wrap_mosquitto_disconnect_callback_set(
    struct mosquitto*, void (*callback)(struct mosquitto*, void*, int)) {
  active->disconnect_callback = callback;
}

extern "C" void __wrap_mosquitto_publish_callback_set(
    struct mosquitto*, void (*callback)(struct mosquitto*, void*, int)) {
  active->publish_callback = callback;
}

extern "C" int __wrap_mosquitto_subscribe(struct mosquitto*, int*,
                                             const char* topic, int qos) {
  active->subscriptions.emplace_back(topic, qos);
  return MOSQ_ERR_SUCCESS;
}

extern "C" int __wrap_mosquitto_publish(struct mosquitto*, int* mid,
                                           const char* topic, int payloadlen,
                                           const void* payload, int qos,
                                           bool retain) {
  const int assigned_mid = active->next_mid++;
  if (mid != nullptr) *mid = assigned_mid;
  active->publications.emplace_back(
      assigned_mid, topic,
      std::string{static_cast<const char*>(payload),
                  static_cast<std::size_t>(payloadlen)},
      qos, retain);
  return MOSQ_ERR_SUCCESS;
}

TEST_CASE("命令主题在连接成功后以QoS2订阅并在重连后重新订阅") {
  std::ostringstream out;
  std::ostringstream err;
  cns::logging::Logger logger(cns::logging::Level::kDebug, out, err);
  Injection injection;
  ScopedInjection scoped{injection};
  auto client = MakeClient(logger);

  REQUIRE(client->SubscribeCommandMessages("cns").has_value());
  injection.connect_callback(nullptr, injection.context, 0);
  CHECK(injection.subscriptions ==
        std::vector<std::tuple<std::string, int>>{
            {"cns/sources/+/config/request", 2}, {"cns/+/config/ack", 2}});

  injection.disconnect_callback(nullptr, injection.context, MOSQ_ERR_CONN_LOST);
  injection.connect_callback(nullptr, injection.context, 0);
  CHECK(injection.subscriptions.size() == 4);
}

TEST_CASE("配置下发与来源ACK均使用QoS2且不保留") {
  std::ostringstream out;
  std::ostringstream err;
  cns::logging::Logger logger(cns::logging::Level::kDebug, out, err);
  Injection injection;
  ScopedInjection scoped{injection};
  auto client = MakeClient(logger);
  injection.connect_callback(nullptr, injection.context, 0);
  REQUIRE(client->ConfigureCommandPublishing([](auto) {}, 2).has_value());

  REQUIRE(client->PublishConfigSet(1, "cns/vendor/config/set", "{}").has_value());
  REQUIRE(client->PublishSourceConfigAck("cns/sources/source/config/ack", "{}")
              .has_value());

  REQUIRE(injection.publications.size() == 2);
  CHECK(std::get<3>(injection.publications[0]) == 2);
  CHECK_FALSE(std::get<4>(injection.publications[0]));
  CHECK(std::get<3>(injection.publications[1]) == 2);
  CHECK_FALSE(std::get<4>(injection.publications[1]));
  REQUIRE(injection.publish_callback != nullptr);
  injection.publish_callback(nullptr, injection.context,
                             std::get<0>(injection.publications[1]));
  CHECK(err.str().find("未知或重复") == std::string::npos);
}

TEST_CASE("MID完成只回调一次并释放token容量") {
  std::ostringstream out;
  std::ostringstream err;
  cns::logging::Logger logger(cns::logging::Level::kDebug, out, err);
  Injection injection;
  ScopedInjection scoped{injection};
  auto client = MakeClient(logger);
  injection.connect_callback(nullptr, injection.context, 0);
  std::vector<cns::mqtt::PublishCompletion> completions;
  REQUIRE(client->ConfigureCommandPublishing(
                    [&](auto completion) {
                      completions.push_back(std::move(completion));
                    },
                    1)
              .has_value());

  REQUIRE(client->PublishConfigSet(7, "cns/vendor/config/set", "{}").has_value());
  CHECK_FALSE(client->PublishConfigSet(8, "cns/vendor/config/set", "{}")
                  .has_value());
  CHECK_FALSE(client->PublishConfigSet(7, "cns/vendor/config/set", "{}")
                  .has_value());

  REQUIRE(injection.publish_callback != nullptr);
  injection.publish_callback(nullptr, injection.context, 10);
  injection.publish_callback(nullptr, injection.context, 10);
  REQUIRE(completions.size() == 1);
  CHECK(completions[0].token == 7);
  CHECK(completions[0].result.has_value());

  CHECK(client->PublishConfigSet(8, "cns/vendor/config/set", "{}").has_value());
}

TEST_CASE("断线把全部在途发布转换为失败完成") {
  std::ostringstream out;
  std::ostringstream err;
  cns::logging::Logger logger(cns::logging::Level::kDebug, out, err);
  Injection injection;
  ScopedInjection scoped{injection};
  auto client = MakeClient(logger);
  injection.connect_callback(nullptr, injection.context, 0);
  std::vector<cns::mqtt::PublishCompletion> completions;
  REQUIRE(client->ConfigureCommandPublishing(
                    [&](auto completion) {
                      completions.push_back(std::move(completion));
                    },
                    2)
              .has_value());
  REQUIRE(client->PublishConfigSet(1, "cns/a/config/set", "{}").has_value());
  REQUIRE(client->PublishConfigSet(2, "cns/b/config/set", "{}").has_value());

  injection.disconnect_callback(nullptr, injection.context, MOSQ_ERR_CONN_LOST);

  REQUIRE(completions.size() == 2);
  CHECK_FALSE(completions[0].result.has_value());
  CHECK_FALSE(completions[1].result.has_value());
}
