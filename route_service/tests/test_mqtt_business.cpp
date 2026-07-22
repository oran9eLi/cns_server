// 本文件验证 MQTT 设备业务消息的订阅、接收与状态事件发布语义。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <mosquitto.h>

#include "adapters/mqtt/mqtt_client.hpp"

#include <algorithm>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

struct Injection {
  void* context = nullptr;
  void (*connect_callback)(struct mosquitto*, void*, int) = nullptr;
  void (*disconnect_callback)(struct mosquitto*, void*, int) = nullptr;
  void (*message_callback)(struct mosquitto*, void*, const struct mosquitto_message*) =
      nullptr;
  std::vector<std::tuple<std::string, int>> subscriptions;
  std::vector<int> subscribe_results{MOSQ_ERR_SUCCESS};
  std::size_t subscribe_attempt = 0;
  std::vector<std::string> unsubscriptions;
  std::vector<std::tuple<std::string, std::string, int, bool>> publications;
  std::mutex stop_mutex;
  std::condition_variable stop_changed;
  std::vector<std::thread::id> loop_stop_threads;
  int connect_result = MOSQ_ERR_SUCCESS;
  std::mutex retry_wait_mutex;
  std::condition_variable retry_wait_changed;
  bool retry_wait_entered = false;
  bool retry_wait_exited = false;
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
extern "C" void __real_mosquitto_disconnect_callback_set(
    struct mosquitto*, void (*)(struct mosquitto*, void*, int));
extern "C" void __real_mosquitto_message_callback_set(
    struct mosquitto*,
    void (*)(struct mosquitto*, void*, const struct mosquitto_message*));
extern "C" int __real_mosquitto_connect_async(struct mosquitto*, const char*,
                                                int, int);
extern "C" int __real_mosquitto_loop_start(struct mosquitto*);
extern "C" int __real_mosquitto_disconnect(struct mosquitto*);
extern "C" int __real_mosquitto_loop_stop(struct mosquitto*, bool);
extern "C" bool __real_cns_runtime_wait_interruptibly(
    std::condition_variable*, std::unique_lock<std::mutex>*,
    std::chrono::seconds, const bool*);

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

extern "C" void __wrap_mosquitto_message_callback_set(
    struct mosquitto*,
    void (*callback)(struct mosquitto*, void*, const struct mosquitto_message*)) {
  active->message_callback = callback;
}

extern "C" int __wrap_mosquitto_subscribe(struct mosquitto*, int*,
                                             const char* topic, int qos) {
  active->subscriptions.emplace_back(topic, qos);
  const auto index = std::min(active->subscribe_attempt,
                              active->subscribe_results.size() - 1);
  ++active->subscribe_attempt;
  return active->subscribe_results[index];
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

extern "C" int __wrap_mosquitto_connect_async(struct mosquitto*, const char*,
                                                int, int) {
  return active->connect_result;
}

extern "C" int __wrap_mosquitto_loop_start(struct mosquitto*) {
  return MOSQ_ERR_SUCCESS;
}

extern "C" int __wrap_mosquitto_disconnect(struct mosquitto*) {
  return MOSQ_ERR_SUCCESS;
}

extern "C" int __wrap_mosquitto_loop_stop(struct mosquitto*, bool) {
  {
    std::lock_guard lock{active->stop_mutex};
    active->loop_stop_threads.push_back(std::this_thread::get_id());
  }
  active->stop_changed.notify_all();
  return MOSQ_ERR_SUCCESS;
}

extern "C" bool __wrap_cns_runtime_wait_interruptibly(
    std::condition_variable* changed, std::unique_lock<std::mutex>* lock,
    std::chrono::seconds delay, const bool* stop_requested) {
  {
    std::lock_guard wait_lock{active->retry_wait_mutex};
    active->retry_wait_entered = true;
  }
  active->retry_wait_changed.notify_all();
  const bool result = __real_cns_runtime_wait_interruptibly(
      changed, lock, delay, stop_requested);
  {
    std::lock_guard wait_lock{active->retry_wait_mutex};
    active->retry_wait_exited = true;
  }
  active->retry_wait_changed.notify_all();
  return result;
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

TEST_CASE("已连接时registration订阅失败同步返回错误且不尝试telemetry") {
  std::ostringstream out;
  std::ostringstream err;
  cns::logging::Logger logger(cns::logging::Level::kDebug, out, err);
  Injection injection;
  ScopedInjection scoped{injection};
  auto client = MakeClient(logger, injection);
  injection.connect_callback(nullptr, injection.context, 0);
  injection.subscribe_results = {MOSQ_ERR_NOMEM};

  const auto subscribed = client->SubscribeDeviceMessages("cns");

  REQUIRE_FALSE(subscribed.has_value());
  CHECK(subscribed.error().find("订阅registration消息失败") !=
        std::string::npos);
  CHECK(injection.subscriptions ==
        std::vector<std::tuple<std::string, int>>{
            {"cns/+/registration", 2}});
}

TEST_CASE("已连接时telemetry订阅失败同步返回错误") {
  std::ostringstream out;
  std::ostringstream err;
  cns::logging::Logger logger(cns::logging::Level::kDebug, out, err);
  Injection injection;
  ScopedInjection scoped{injection};
  auto client = MakeClient(logger, injection);
  injection.connect_callback(nullptr, injection.context, 0);
  injection.subscribe_results = {MOSQ_ERR_SUCCESS, MOSQ_ERR_NOMEM};

  const auto subscribed = client->SubscribeDeviceMessages("cns");

  REQUIRE_FALSE(subscribed.has_value());
  CHECK(subscribed.error().find("订阅telemetry消息失败") != std::string::npos);
  CHECK(injection.subscriptions ==
        std::vector<std::tuple<std::string, int>>{
            {"cns/+/registration", 2}, {"cns/+/telemetry", 0}});
}

TEST_CASE("同一连接代仅成功订阅一次且新连接代重新订阅") {
  std::ostringstream out;
  std::ostringstream err;
  cns::logging::Logger logger(cns::logging::Level::kDebug, out, err);
  Injection injection;
  ScopedInjection scoped{injection};
  auto client = MakeClient(logger, injection);
  REQUIRE(client->SubscribeDeviceMessages("cns").has_value());

  injection.connect_callback(nullptr, injection.context, 0);
  injection.connect_callback(nullptr, injection.context, 0);
  CHECK(injection.subscriptions.size() == 2);

  REQUIRE(injection.disconnect_callback != nullptr);
  injection.disconnect_callback(nullptr, injection.context, MOSQ_ERR_CONN_LOST);
  injection.connect_callback(nullptr, injection.context, 0);
  CHECK(injection.subscriptions.size() == 4);
}

TEST_CASE("订阅失败不标记当前连接代并允许完整重试") {
  std::ostringstream out;
  std::ostringstream err;
  cns::logging::Logger logger(cns::logging::Level::kDebug, out, err);
  Injection injection;
  injection.subscribe_results = {MOSQ_ERR_SUCCESS, MOSQ_ERR_NOMEM,
                                 MOSQ_ERR_SUCCESS};
  ScopedInjection scoped{injection};
  auto client = MakeClient(logger, injection);
  REQUIRE(client->SubscribeDeviceMessages("cns").has_value());

  injection.connect_callback(nullptr, injection.context, 0);
  injection.connect_callback(nullptr, injection.context, 0);

  CHECK(injection.subscriptions ==
        std::vector<std::tuple<std::string, int>>{
            {"cns/+/registration", 2}, {"cns/+/telemetry", 0},
            {"cns/+/registration", 2}, {"cns/+/telemetry", 0}});
  CHECK(err.str().find("MQTT业务订阅失败：订阅telemetry消息") !=
        std::string::npos);
}

TEST_CASE("连接成功后业务订阅失败可通过补订阅入口恢复") {
  std::ostringstream out;
  std::ostringstream err;
  cns::logging::Logger logger(cns::logging::Level::kDebug, out, err);
  Injection injection;
  injection.subscribe_results = {MOSQ_ERR_SUCCESS, MOSQ_ERR_NOMEM,
                                 MOSQ_ERR_SUCCESS, MOSQ_ERR_SUCCESS};
  ScopedInjection scoped{injection};
  auto client = MakeClient(logger, injection);
  REQUIRE(client->SubscribeDeviceMessages("cns").has_value());

  injection.connect_callback(nullptr, injection.context, 0);
  CHECK(injection.subscriptions.size() == 2);
  CHECK(err.str().find("MQTT业务订阅失败：订阅telemetry消息") !=
        std::string::npos);

  REQUIRE(client->EnsureBusinessSubscriptions().has_value());
  CHECK(injection.subscriptions ==
        std::vector<std::tuple<std::string, int>>{
            {"cns/+/registration", 2}, {"cns/+/telemetry", 0},
            {"cns/+/registration", 2}, {"cns/+/telemetry", 0}});
  CHECK(out.str().find("MQTT业务订阅已恢复") != std::string::npos);
}

TEST_CASE("业务订阅已恢复后补订阅入口幂等") {
  std::ostringstream out;
  std::ostringstream err;
  cns::logging::Logger logger(cns::logging::Level::kDebug, out, err);
  Injection injection;
  ScopedInjection scoped{injection};
  auto client = MakeClient(logger, injection);
  REQUIRE(client->SubscribeDeviceMessages("cns").has_value());

  injection.connect_callback(nullptr, injection.context, 0);
  REQUIRE(client->EnsureBusinessSubscriptions().has_value());

  CHECK(injection.subscriptions ==
        std::vector<std::tuple<std::string, int>>{
            {"cns/+/registration", 2}, {"cns/+/telemetry", 0}});
}

TEST_CASE("替换handler在状态锁外析构旧capture") {
  std::ostringstream out;
  std::ostringstream err;
  cns::logging::Logger logger(cns::logging::Level::kDebug, out, err);
  Injection injection;
  ScopedInjection scoped{injection};
  auto client = MakeClient(logger, injection);
  std::atomic_bool destroyed{false};
  struct ReenterOnDestroy {
    cns::mqtt::MqttClient* client;
    std::atomic_bool* destroyed;
    ~ReenterOnDestroy() {
      static_cast<void>(client->ConfigureBusinessMessages(
          [](cns::mqtt::InboundMessage) {}, 16));
      destroyed->store(true, std::memory_order_release);
    }
  };
  auto capture =
      std::make_shared<ReenterOnDestroy>(client.get(), &destroyed);
  REQUIRE(client->ConfigureBusinessMessages(
                    [capture](cns::mqtt::InboundMessage) {}, 16)
              .has_value());
  capture.reset();

  std::thread replacement([&] {
    static_cast<void>(client->ConfigureBusinessMessages(
        [](cns::mqtt::InboundMessage) {}, 16));
  });
  const auto deadline = std::chrono::steady_clock::now() + 200ms;
  while (!destroyed.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }

  CHECK(destroyed.load(std::memory_order_acquire));
  if (destroyed.load(std::memory_order_acquire)) {
    replacement.join();
  } else {
    replacement.detach();
    static_cast<void>(client.release());
  }
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

TEST_CASE("空handler用于原子停止接收设备消息") {
  std::ostringstream out;
  std::ostringstream err;
  cns::logging::Logger logger(cns::logging::Level::kDebug, out, err);
  Injection injection;
  ScopedInjection scoped{injection};
  auto client = MakeClient(logger, injection);
  int calls = 0;
  REQUIRE(client->ConfigureBusinessMessages(
                    [&](cns::mqtt::InboundMessage) { ++calls; }, 64)
              .has_value());
  REQUIRE(client->ConfigureBusinessMessages({}, 64).has_value());
  std::string payload = "{}";
  mosquitto_message message{.mid = 1,
                            .topic = const_cast<char*>("cns/x/telemetry"),
                            .payload = payload.data(),
                            .payloadlen = 2,
                            .qos = 0,
                            .retain = false};

  injection.message_callback(nullptr, injection.context, &message);
  CHECK(calls == 0);
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

TEST_CASE("handler内Stop仅请求且由后续外部Stop实际执行") {
  std::ostringstream out;
  std::ostringstream err;
  cns::logging::Logger logger(cns::logging::Level::kDebug, out, err);
  Injection injection;
  ScopedInjection scoped{injection};
  auto client = MakeClient(logger, injection);
  REQUIRE(client->Start().has_value());
  REQUIRE(client->ConfigureBusinessMessages(
                    [&](cns::mqtt::InboundMessage) { client->Stop(); }, 64)
              .has_value());
  std::string payload = "{}";
  mosquitto_message message{.mid = 1,
                            .topic = const_cast<char*>("cns/x/telemetry"),
                            .payload = payload.data(),
                            .payloadlen = 2,
                            .qos = 0,
                            .retain = false};

  injection.message_callback(nullptr, injection.context, &message);
  CHECK(client->CallbackStopRequested());
  {
    std::unique_lock lock{injection.stop_mutex};
    CHECK_FALSE(injection.stop_changed.wait_for(lock, 100ms, [&] {
      return !injection.loop_stop_threads.empty();
    }));
  }
  const auto restarted = client->Start();
  REQUIRE_FALSE(restarted.has_value());
  CHECK(restarted.error().find("请先由外部线程调用Stop") != std::string::npos);
  client->Stop();
  CHECK_FALSE(client->CallbackStopRequested());
  CHECK(injection.loop_stop_threads.size() == 1);
}

TEST_CASE("并发回调重复Stop仅合并请求且外部Stop执行一次") {
  std::ostringstream out;
  std::ostringstream err;
  cns::logging::Logger logger(cns::logging::Level::kDebug, out, err);
  Injection injection;
  ScopedInjection scoped{injection};
  auto client = MakeClient(logger, injection);
  REQUIRE(client->Start().has_value());
  REQUIRE(client->ConfigureBusinessMessages(
                    [&](cns::mqtt::InboundMessage) { client->Stop(); }, 64)
              .has_value());
  std::string payload = "{}";
  mosquitto_message message{.mid = 1,
                            .topic = const_cast<char*>("cns/x/telemetry"),
                            .payload = payload.data(),
                            .payloadlen = 2,
                            .qos = 0,
                            .retain = false};

  std::thread first([&] {
    injection.message_callback(nullptr, injection.context, &message);
  });
  std::thread second([&] {
    injection.message_callback(nullptr, injection.context, &message);
  });
  first.join();
  second.join();
  CHECK(client->CallbackStopRequested());
  {
    std::unique_lock lock{injection.stop_mutex};
    CHECK_FALSE(injection.stop_changed.wait_for(lock, 100ms, [&] {
      return !injection.loop_stop_threads.empty();
    }));
  }
  client->Stop();
  client->Stop();
  CHECK_FALSE(client->CallbackStopRequested());
  CHECK(injection.loop_stop_threads.size() == 1);
}

TEST_CASE("handler内释放最后client owner安全保活并禁用后续handler") {
  std::ostringstream out;
  std::ostringstream err;
  cns::logging::Logger logger(cns::logging::Level::kDebug, out, err);
  Injection injection;
  ScopedInjection scoped{injection};
  std::shared_ptr<cns::mqtt::MqttClient> client = MakeClient(logger, injection);
  std::weak_ptr<cns::mqtt::MqttClient> weak_client = client;
  auto last_owner =
      std::make_shared<std::shared_ptr<cns::mqtt::MqttClient>>(client);
  int handler_calls = 0;
  REQUIRE(client->ConfigureBusinessMessages(
                    [last_owner, &handler_calls](cns::mqtt::InboundMessage) {
                      ++handler_calls;
                      last_owner->reset();
                    },
                    64)
              .has_value());
  client.reset();
  std::string payload = "{}";
  mosquitto_message message{.mid = 1,
                            .topic = const_cast<char*>("cns/x/telemetry"),
                            .payload = payload.data(),
                            .payloadlen = 2,
                            .qos = 0,
                            .retain = false};

  injection.message_callback(nullptr, injection.context, &message);

  CHECK(weak_client.expired());
  CHECK(handler_calls == 1);
  injection.message_callback(nullptr, injection.context, &message);
  CHECK(handler_calls == 1);
}

TEST_CASE("EAI重试等待期间回调内析构先中断并回收重试线程") {
  std::ostringstream out;
  std::ostringstream err;
  cns::logging::Logger logger(cns::logging::Level::kDebug, out, err);
  Injection injection;
  injection.connect_result = MOSQ_ERR_EAI;
  ScopedInjection scoped{injection};
  std::shared_ptr<cns::mqtt::MqttClient> client = MakeClient(logger, injection);
  std::weak_ptr<cns::mqtt::MqttClient> weak_client = client;
  auto last_owner =
      std::make_shared<std::shared_ptr<cns::mqtt::MqttClient>>(client);
  REQUIRE(client->ConfigureBusinessMessages(
                    [last_owner](cns::mqtt::InboundMessage) {
                      last_owner->reset();
                    },
                    64)
              .has_value());
  REQUIRE(client->Start().has_value());
  {
    std::unique_lock lock{injection.retry_wait_mutex};
    REQUIRE(injection.retry_wait_changed.wait_for(lock, 500ms, [&] {
      return injection.retry_wait_entered;
    }));
  }
  client.reset();
  std::string payload = "{}";
  mosquitto_message message{.mid = 1,
                            .topic = const_cast<char*>("cns/x/telemetry"),
                            .payload = payload.data(),
                            .payloadlen = 2,
                            .qos = 0,
                            .retain = false};

  injection.message_callback(nullptr, injection.context, &message);

  CHECK(weak_client.expired());
  CHECK(injection.retry_wait_exited);
}

TEST_CASE("回调内放弃路径catch不无锁写logger") {
  std::ifstream source{CNS_MQTT_CLIENT_SOURCE_FILE};
  REQUIRE(source.good());
  const std::string text{std::istreambuf_iterator<char>{source},
                         std::istreambuf_iterator<char>{}};
  CHECK(text.find("catch (...) {\n      logger = nullptr;") ==
        std::string::npos);
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

TEST_CASE("成功replay不把仅registration恢复误标为完整业务订阅") {
  std::ostringstream out;
  std::ostringstream err;
  cns::logging::Logger logger(cns::logging::Level::kDebug, out, err);
  Injection injection;
  injection.subscribe_results = {MOSQ_ERR_NOMEM, MOSQ_ERR_SUCCESS};
  ScopedInjection scoped{injection};
  auto client = MakeClient(logger, injection);
  REQUIRE(client->SubscribeDeviceMessages("cns").has_value());
  injection.connect_callback(nullptr, injection.context, 0);
  REQUIRE(injection.subscriptions.size() == 1);

  REQUIRE(client->ReplayRetainedRegistrations("cns").has_value());
  REQUIRE(injection.subscriptions.size() == 2);
  injection.connect_callback(nullptr, injection.context, 0);

  CHECK(injection.subscriptions.size() == 4);
}
