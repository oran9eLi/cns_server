// 本文件实现 libmosquitto 全局库租约、异步连接、自动重连和安全停止。
#include "adapters/mqtt/mqtt_client.hpp"

#include <mosquitto.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "core/mqtt_topic/device_topic.hpp"

extern "C" bool cns_runtime_wait_interruptibly(
    std::condition_variable* changed, std::unique_lock<std::mutex>* lock,
    std::chrono::seconds delay, const bool* stop_requested);

namespace cns::mqtt {
namespace {

thread_local const void* current_callback_state = nullptr;

struct LibraryState {
  std::mutex mutex;
  std::size_t users = 0;
};

LibraryState& GlobalLibraryState() {
  static LibraryState* state = new LibraryState;
  return *state;
}

int AcquireLibrary() {
  auto& state = GlobalLibraryState();
  std::lock_guard lock{state.mutex};
  if (state.users == 0) {
    const int result = mosquitto_lib_init();
    if (result != MOSQ_ERR_SUCCESS) return result;
  }
  ++state.users;
  return MOSQ_ERR_SUCCESS;
}

void ReleaseLibrary() {
  auto& state = GlobalLibraryState();
  std::lock_guard lock{state.mutex};
  if (state.users == 0) return;
  --state.users;
  if (state.users == 0) mosquitto_lib_cleanup();
}

std::string MosquittoError(std::string_view operation, int result) {
  return std::string{operation} + "失败，错误码=" + std::to_string(result) +
         "，原因=" + mosquitto_strerror(result);
}

}  // namespace

struct MqttClient::CallbackState {
  explicit CallbackState(logging::Logger& logger_value)
      : logger(&logger_value),
        disconnect_warnings(std::chrono::seconds{30}),
        library_logs(std::chrono::seconds{30}),
        business_warnings(std::chrono::seconds{30}) {}

  void DisableLogger() {
    std::lock_guard lock{mutex};
    logger = nullptr;
  }

  void WarnDisconnected(int result) {
    std::lock_guard lock{mutex};
    if (logger == nullptr ||
        !disconnect_warnings.ShouldEmit("mqtt_disconnect",
                                        runtime::RateLimiter::Clock::now())) {
      return;
    }
    logger->Warn("MQTT连接中断，将自动重连，错误码=" +
                 std::to_string(result));
  }

  void WarnLibraryEvent(std::string_view category, std::string_view text) {
    std::lock_guard lock{mutex};
    if (logger == nullptr ||
        !library_logs.ShouldEmit(category, runtime::RateLimiter::Clock::now())) {
      return;
    }
    logger->Warn(text);
  }

  void Warn(std::string message) {
    std::lock_guard lock{mutex};
    if (logger != nullptr) logger->Warn(message);
  }

  void Connected() {
    std::lock_guard lock{mutex};
    if (!connected.exchange(true, std::memory_order_acq_rel)) {
      ++connection_generation;
    }
    if (logger != nullptr) logger->Info("MQTT连接已恢复");
  }

  std::expected<void, std::string> SubscribeConfigured(
      struct mosquitto* client) {
    std::lock_guard subscription_lock{subscription_mutex};
    std::string device_namespace;
    std::string command_namespace;
    std::size_t generation = 0;
    bool subscribe_devices = false;
    bool subscribe_commands = false;
    {
      std::lock_guard lock{mutex};
      device_namespace = device_topic_namespace;
      command_namespace = command_topic_namespace;
      generation = connection_generation;
      subscribe_devices =
          !device_namespace.empty() &&
          (registration_generation != generation ||
           telemetry_generation != generation);
      subscribe_commands =
          !command_namespace.empty() &&
          (source_request_generation != generation ||
           device_ack_generation != generation);
    }
    if (subscribe_devices) {
      const auto registration = mqtt_topic::RegistrationFilter(device_namespace);
      const int result = mosquitto_subscribe(client, nullptr,
                                             registration.c_str(), 2);
      if (result != MOSQ_ERR_SUCCESS) {
        return std::unexpected(MosquittoError("订阅registration消息", result));
      }
    }
    if (subscribe_devices) {
      const auto telemetry = mqtt_topic::TelemetryFilter(device_namespace);
      const int result =
          mosquitto_subscribe(client, nullptr, telemetry.c_str(), 0);
      if (result != MOSQ_ERR_SUCCESS) {
        return std::unexpected(MosquittoError("订阅telemetry消息", result));
      }
    }
    if (subscribe_commands) {
      const auto topic = command_namespace + "/sources/+/config/request";
      const int result = mosquitto_subscribe(client, nullptr, topic.c_str(), 2);
      if (result != MOSQ_ERR_SUCCESS) {
        return std::unexpected(MosquittoError("订阅配置命令请求", result));
      }
    }
    if (subscribe_commands) {
      const auto topic = command_namespace + "/+/config/ack";
      const int result = mosquitto_subscribe(client, nullptr, topic.c_str(), 2);
      if (result != MOSQ_ERR_SUCCESS) {
        return std::unexpected(MosquittoError("订阅设备配置ACK", result));
      }
    }
    if (subscribe_commands) {
      const auto topic = command_namespace + "/sources/+/control/request";
      const int result = mosquitto_subscribe(client, nullptr, topic.c_str(), 2);
      if (result != MOSQ_ERR_SUCCESS) {
        return std::unexpected(MosquittoError("订阅飞控命令请求", result));
      }
    }
    if (subscribe_commands) {
      const auto topic = command_namespace + "/+/control/ack";
      const int result = mosquitto_subscribe(client, nullptr, topic.c_str(), 2);
      if (result != MOSQ_ERR_SUCCESS) {
        return std::unexpected(MosquittoError("订阅设备飞控ACK", result));
      }
    }
    {
      std::lock_guard lock{mutex};
      if (connected.load(std::memory_order_acquire) &&
          connection_generation == generation) {
        if (device_topic_namespace == device_namespace &&
            !device_namespace.empty()) {
          registration_generation = generation;
          telemetry_generation = generation;
        }
        if (command_topic_namespace == command_namespace &&
            !command_namespace.empty()) {
          source_request_generation = generation;
          device_ack_generation = generation;
        }
        if (logger != nullptr && (subscribe_devices || subscribe_commands)) {
          logger->Info("MQTT业务订阅已恢复");
        }
      }
    }
    return {};
  }

  std::optional<std::size_t> BusinessSubscriptionGeneration() {
    std::lock_guard lock{mutex};
    if (!connected.load(std::memory_order_acquire)) return std::nullopt;
    const bool has_device_subscriptions = !device_topic_namespace.empty();
    const bool has_command_subscriptions = !command_topic_namespace.empty();
    if (!has_device_subscriptions && !has_command_subscriptions) {
      return std::nullopt;
    }
    if (has_device_subscriptions &&
        (registration_generation != connection_generation ||
         telemetry_generation != connection_generation)) {
      return std::nullopt;
    }
    if (has_command_subscriptions &&
        (source_request_generation != connection_generation ||
         device_ack_generation != connection_generation)) {
      return std::nullopt;
    }
    return connection_generation;
  }

  void Dispatch(const struct mosquitto_message* message,
                std::chrono::system_clock::time_point received_at) {
    MessageHandler configured_handler;
    std::size_t configured_limit = 0;
    {
      std::lock_guard lock{mutex};
      configured_handler = handler;
      configured_limit = max_payload_bytes;
    }
    if (!configured_handler || message == nullptr || message->topic == nullptr ||
        message->payloadlen < 0) {
      return;
    }
    const auto payload_size = static_cast<std::size_t>(message->payloadlen);
    if (payload_size > configured_limit) {
      WarnBusiness("mqtt_payload_too_large",
                   "MQTT消息payload超过大小限制，已拒绝");
      return;
    }
    if (payload_size != 0 && message->payload == nullptr) return;
    std::string payload;
    if (payload_size != 0) {
      payload.assign(static_cast<const char*>(message->payload), payload_size);
    }
    InboundMessage inbound{.topic = message->topic,
                           .payload = std::move(payload),
                           .received_at = received_at};
    try {
      configured_handler(std::move(inbound));
    } catch (...) {
      WarnBusiness("mqtt_business_handler", "MQTT业务消息处理器异常");
    }
  }

  void WarnBusiness(std::string_view category, std::string_view text) {
    std::lock_guard lock{mutex};
    if (logger != nullptr && business_warnings.ShouldEmit(
                                 category, runtime::RateLimiter::Clock::now())) {
      logger->Error(text);
    }
  }

  bool IsCurrentCallback() const noexcept {
    return current_callback_state == this;
  }

  void AbandonFromCallback() noexcept {
    MessageHandler retired_handler;
    PublishCompletionHandler retired_publish_handler;
    try {
      std::lock_guard lock{mutex};
      logger = nullptr;
      handler.swap(retired_handler);
      publish_handler.swap(retired_publish_handler);
      token_to_mid.clear();
      mid_to_token.clear();
      untracked_completion_mids.clear();
      device_topic_namespace.clear();
      command_topic_namespace.clear();
    } catch (...) {
      return;
    }
  }

  std::mutex mutex;
  std::mutex subscription_mutex;
  logging::Logger* logger;
  runtime::RateLimiter disconnect_warnings;
  runtime::RateLimiter library_logs;
  runtime::RateLimiter business_warnings;
  std::atomic_bool connected{false};
  MessageHandler handler;
  PublishCompletionHandler publish_handler;
  std::size_t max_payload_bytes = 0;
  std::size_t publish_capacity = 0;
  std::unordered_map<std::uint64_t, int> token_to_mid;
  std::unordered_map<int, std::uint64_t> mid_to_token;
  std::unordered_set<int> untracked_completion_mids;
  std::string device_topic_namespace;
  std::string command_topic_namespace;
  std::size_t connection_generation = 0;
  std::optional<std::size_t> registration_generation;
  std::optional<std::size_t> telemetry_generation;
  std::optional<std::size_t> source_request_generation;
  std::optional<std::size_t> device_ack_generation;
};

struct MqttClient::CallbackGuard {
  explicit CallbackGuard(CallbackState& state_value) noexcept
      : previous(current_callback_state) {
    current_callback_state = &state_value;
  }

  ~CallbackGuard() noexcept {
    current_callback_state = previous;
  }

  const void* previous;
};

MqttClient::MqttClient(config::MqttConfig config, logging::Logger& logger)
    : config_(std::move(config)),
      callback_state_(std::make_unique<CallbackState>(logger)) {}

std::expected<std::unique_ptr<MqttClient>, std::string> MqttClient::Create(
    const config::MqttConfig& config, logging::Logger& logger) {
  auto instance = std::unique_ptr<MqttClient>{new MqttClient(config, logger)};
  const int library_result = AcquireLibrary();
  if (library_result != MOSQ_ERR_SUCCESS) {
    return std::unexpected(MosquittoError("初始化MQTT库", library_result));
  }
  instance->library_acquired_ = true;

  instance->client_ = mosquitto_new(config.client_id.c_str(), true,
                                    instance->callback_state_.get());
  if (instance->client_ == nullptr) {
    return std::unexpected("创建MQTT客户端失败");
  }

  if (!config.username.empty()) {
    const int credentials_result = mosquitto_username_pw_set(
        instance->client_, config.username.c_str(), config.password.c_str());
    if (credentials_result != MOSQ_ERR_SUCCESS) {
      return std::unexpected(
          MosquittoError("设置MQTT认证信息", credentials_result));
    }
  }

  mosquitto_connect_callback_set(instance->client_, &MqttClient::HandleConnect);
  mosquitto_disconnect_callback_set(instance->client_,
                                    &MqttClient::HandleDisconnect);
  mosquitto_log_callback_set(instance->client_, &MqttClient::HandleLog);
  mosquitto_message_callback_set(instance->client_, &MqttClient::HandleMessage);
  mosquitto_publish_callback_set(instance->client_, &MqttClient::HandlePublish);

  const int reconnect_result = mosquitto_reconnect_delay_set(
      instance->client_, static_cast<unsigned int>(config.reconnect_delay.count()),
      static_cast<unsigned int>(config.reconnect_delay_max.count()), true);
  if (reconnect_result != MOSQ_ERR_SUCCESS) {
    return std::unexpected(
        MosquittoError("设置MQTT重连参数", reconnect_result));
  }
  return instance;
}

MqttClient::~MqttClient() {
  if (callback_state_ != nullptr && callback_state_->IsCurrentCallback()) {
    {
      std::lock_guard retry_lock{retry_mutex_};
      retry_stop_requested_ = true;
    }
    retry_changed_.notify_all();
    callback_state_->AbandonFromCallback();
    if (retry_thread_.joinable() &&
        retry_thread_.get_id() != std::this_thread::get_id()) {
      retry_thread_.join();
    }
    static_cast<void>(callback_state_.release());
    client_ = nullptr;
    library_acquired_ = false;
    return;
  }
  Stop();
  if (loop_started_.load(std::memory_order_acquire)) {
    callback_state_->DisableLogger();
    static_cast<void>(callback_state_.release());
    client_ = nullptr;
    library_acquired_ = false;
    return;
  }
  if (client_ != nullptr) {
    mosquitto_destroy(client_);
    client_ = nullptr;
  }
  if (library_acquired_) {
    ReleaseLibrary();
    library_acquired_ = false;
  }
}

std::expected<void, std::string> MqttClient::Start() {
  std::lock_guard lock{lifecycle_mutex_};
  if (callback_stop_requested_.load(std::memory_order_acquire)) {
    return std::unexpected("MQTT回调已请求停止，请先由外部线程调用Stop");
  }
  if (loop_started_.load(std::memory_order_acquire)) return {};

  std::string completed_error;
  {
    std::lock_guard retry_lock{retry_mutex_};
    if (retry_state_ == RetryState::kRunning) return {};
    if (retry_state_ == RetryState::kFailed) {
      completed_error = std::move(retry_error_);
      retry_state_ = RetryState::kIdle;
    }
  }
  if (retry_thread_.joinable()) retry_thread_.join();
  if (!completed_error.empty()) return std::unexpected(completed_error);

  {
    std::lock_guard retry_lock{retry_mutex_};
    retry_stop_requested_ = false;
  }

  const int connect_result = mosquitto_connect_async(
      client_, config_.host.c_str(), static_cast<int>(config_.port),
      static_cast<int>(config_.keepalive.count()));
  const bool initial_network_failure =
      connect_result == MOSQ_ERR_ERRNO || connect_result == MOSQ_ERR_EAI;
  if (connect_result != MOSQ_ERR_SUCCESS && !initial_network_failure) {
    return std::unexpected(MosquittoError("启动MQTT异步连接", connect_result));
  }
  if (initial_network_failure) WarnDisconnected(connect_result);

  if (connect_result == MOSQ_ERR_EAI) {
    {
      std::lock_guard retry_lock{retry_mutex_};
      retry_state_ = RetryState::kRunning;
      retry_error_.clear();
    }
    try {
      retry_thread_ = std::thread(&MqttClient::RetryConnection, this);
    } catch (const std::system_error& error) {
      FinishRetry();
      return std::unexpected(std::string{"启动MQTT连接重试线程失败："} +
                             error.what());
    }
    return {};
  }

  const int loop_result = mosquitto_loop_start(client_);
  if (loop_result != MOSQ_ERR_SUCCESS) {
    static_cast<void>(mosquitto_disconnect(client_));
    return std::unexpected(MosquittoError("启动MQTT网络线程", loop_result));
  }
  loop_started_.store(true, std::memory_order_release);
  return {};
}

void MqttClient::Stop() {
  if (callback_state_->IsCurrentCallback()) {
    callback_stop_requested_.store(true, std::memory_order_release);
    return;
  }
  StopNow();
}

bool MqttClient::CallbackStopRequested() const noexcept {
  return callback_stop_requested_.load(std::memory_order_acquire);
}

void MqttClient::StopNow() {
  std::lock_guard lock{lifecycle_mutex_};
  {
    std::lock_guard retry_lock{retry_mutex_};
    retry_stop_requested_ = true;
  }
  retry_changed_.notify_all();
  if (retry_thread_.joinable()) retry_thread_.join();
  {
    std::lock_guard retry_lock{retry_mutex_};
    retry_state_ = RetryState::kIdle;
    retry_error_.clear();
  }

  if (!loop_started_.load(std::memory_order_acquire)) {
    callback_state_->connected.store(false, std::memory_order_release);
    callback_stop_requested_.store(false, std::memory_order_release);
    return;
  }

  const int disconnect_result = mosquitto_disconnect(client_);
  if (disconnect_result != MOSQ_ERR_SUCCESS &&
      disconnect_result != MOSQ_ERR_NO_CONN) {
    callback_state_->Warn(MosquittoError("请求MQTT断开", disconnect_result));
  }

  const int loop_stop_result = mosquitto_loop_stop(client_, false);
  if (loop_stop_result != MOSQ_ERR_SUCCESS) {
    callback_state_->Warn(
        MosquittoError("停止MQTT网络线程", loop_stop_result));
  } else {
    loop_started_.store(false, std::memory_order_release);
  }
  callback_state_->connected.store(false, std::memory_order_release);
  if (!loop_started_.load(std::memory_order_acquire)) {
    callback_stop_requested_.store(false, std::memory_order_release);
  }
}

bool MqttClient::IsConnected() const noexcept {
  return callback_state_->connected.load(std::memory_order_acquire);
}

mqtt::RuntimeStatus MqttClient::GetRuntimeStatus() const noexcept {
  {
    std::lock_guard lock{retry_mutex_};
    if (retry_state_ == RetryState::kFailed) {
      return mqtt::RuntimeStatus::kTerminalFailure;
    }
  }
  return IsConnected() ? mqtt::RuntimeStatus::kConnected
                       : mqtt::RuntimeStatus::kDisconnectedOrRetrying;
}

std::expected<void, std::string> MqttClient::ConfigureBusinessMessages(
    MessageHandler handler, std::size_t max_payload_bytes) {
  if (max_payload_bytes == 0) {
    return std::unexpected("MQTT payload大小限制必须大于零");
  }
  MessageHandler replacement = std::move(handler);
  {
    std::lock_guard lock{callback_state_->mutex};
    callback_state_->handler.swap(replacement);
    callback_state_->max_payload_bytes = max_payload_bytes;
  }
  return {};
}

std::expected<void, std::string> MqttClient::SubscribeDeviceMessages(
    std::string_view topic_namespace) {
  if (topic_namespace.empty()) return std::unexpected("MQTT topic命名空间不能为空");
  {
    std::lock_guard lock{callback_state_->mutex};
    if (callback_state_->device_topic_namespace != topic_namespace) {
      callback_state_->registration_generation.reset();
      callback_state_->telemetry_generation.reset();
    }
    callback_state_->device_topic_namespace = topic_namespace;
  }
  if (IsConnected()) return callback_state_->SubscribeConfigured(client_);
  return {};
}

std::expected<void, std::string> MqttClient::EnsureBusinessSubscriptions() {
  if (IsConnected()) return callback_state_->SubscribeConfigured(client_);
  return {};
}

std::optional<std::size_t> MqttClient::BusinessSubscriptionGeneration() const {
  return callback_state_->BusinessSubscriptionGeneration();
}

std::expected<void, std::string> MqttClient::ReplayRetainedRegistrations(
    std::string_view topic_namespace) {
  if (topic_namespace.empty()) return std::unexpected("MQTT topic命名空间不能为空");
  std::lock_guard subscription_lock{callback_state_->subscription_mutex};
  const auto filter = mqtt_topic::RegistrationFilter(topic_namespace);
  const int unsubscribe_result =
      mosquitto_unsubscribe(client_, nullptr, filter.c_str());
  if (unsubscribe_result != MOSQ_ERR_SUCCESS) {
    return std::unexpected(MosquittoError("取消registration订阅", unsubscribe_result));
  }
  const int subscribe_result = mosquitto_subscribe(client_, nullptr,
                                                    filter.c_str(), 2);
  if (subscribe_result != MOSQ_ERR_SUCCESS) {
    return std::unexpected(MosquittoError("恢复registration订阅", subscribe_result));
  }
  {
    std::lock_guard lock{callback_state_->mutex};
    if (callback_state_->connected.load(std::memory_order_acquire) &&
        callback_state_->device_topic_namespace == topic_namespace) {
      callback_state_->registration_generation =
          callback_state_->connection_generation;
    }
  }
  return {};
}

std::expected<void, std::string> MqttClient::ConfigureCommandPublishing(
    PublishCompletionHandler handler, std::size_t capacity) {
  if (capacity == 0) return std::unexpected("MQTT命令发布容量必须大于零");
  PublishCompletionHandler retired;
  {
    std::lock_guard lock{callback_state_->mutex};
    callback_state_->publish_handler.swap(retired);
    callback_state_->publish_handler = std::move(handler);
    callback_state_->publish_capacity = capacity;
    callback_state_->token_to_mid.clear();
    callback_state_->mid_to_token.clear();
    callback_state_->untracked_completion_mids.clear();
  }
  return {};
}

std::expected<void, std::string> MqttClient::SubscribeCommandMessages(
    std::string_view topic_namespace) {
  if (topic_namespace.empty()) return std::unexpected("MQTT topic命名空间不能为空");
  {
    std::lock_guard lock{callback_state_->mutex};
    if (callback_state_->command_topic_namespace != topic_namespace) {
      callback_state_->source_request_generation.reset();
      callback_state_->device_ack_generation.reset();
    }
    callback_state_->command_topic_namespace = topic_namespace;
  }
  if (IsConnected()) return callback_state_->SubscribeConfigured(client_);
  return {};
}

std::expected<void, std::string> MqttClient::PublishCommandSet(
    std::uint64_t token, std::string_view topic, std::string_view payload) {
  if (topic.empty()) return std::unexpected("MQTT发布topic不能为空");
  if (payload.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return std::unexpected("MQTT发布payload过大");
  }
  std::lock_guard lock{callback_state_->mutex};
  if (!callback_state_->publish_handler) {
    return std::unexpected("MQTT命令发布处理器未配置");
  }
  if (callback_state_->token_to_mid.contains(token)) {
    return std::unexpected("MQTT命令发布token已在途");
  }
  if (callback_state_->token_to_mid.size() >= callback_state_->publish_capacity) {
    return std::unexpected("MQTT命令发布容量已满");
  }
  int mid = 0;
  const int result = mosquitto_publish(
      client_, &mid, std::string{topic}.c_str(), static_cast<int>(payload.size()),
      payload.data(), 2, false);
  if (result != MOSQ_ERR_SUCCESS) {
    return std::unexpected(MosquittoError("发布设备命令", result));
  }
  callback_state_->token_to_mid.emplace(token, mid);
  callback_state_->mid_to_token.emplace(mid, token);
  return {};
}

std::expected<void, std::string> MqttClient::PublishSourceCommandAck(
    std::string_view topic, std::string_view payload) {
  if (topic.empty()) return std::unexpected("MQTT发布topic不能为空");
  if (payload.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return std::unexpected("MQTT发布payload过大");
  }
  std::lock_guard lock{callback_state_->mutex};
  int mid = 0;
  const int result = mosquitto_publish(
      client_, &mid, std::string{topic}.c_str(), static_cast<int>(payload.size()),
      payload.data(), 2, false);
  if (result != MOSQ_ERR_SUCCESS) {
    return std::unexpected(MosquittoError("发布来源命令ACK", result));
  }
  callback_state_->untracked_completion_mids.insert(mid);
  return {};
}

std::expected<void, std::string> MqttClient::PublishStateEvent(
    std::string_view topic, std::string_view payload) {
  if (topic.empty()) return std::unexpected("MQTT发布topic不能为空");
  if (payload.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return std::unexpected("MQTT发布payload过大");
  }
  std::lock_guard lock{callback_state_->mutex};
  int mid = 0;
  const int result = mosquitto_publish(
      client_, &mid, std::string{topic}.c_str(), static_cast<int>(payload.size()),
      payload.data(), 1, true);
  if (result != MOSQ_ERR_SUCCESS) {
    return std::unexpected(MosquittoError("发布设备当前状态快照", result));
  }
  callback_state_->untracked_completion_mids.insert(mid);
  return {};
}

void MqttClient::HandleConnect(struct mosquitto* client, void* context,
                               int result) noexcept {
  auto& state = *static_cast<CallbackState*>(context);
  try {
    CallbackGuard callback_guard{state};
    if (result == 0) {
      state.Connected();
      const auto subscribed = state.SubscribeConfigured(client);
      if (!subscribed.has_value()) {
        state.WarnBusiness("mqtt_business_subscribe",
                           "MQTT业务订阅失败：" + subscribed.error());
      }
      return;
    }
    state.connected.store(false, std::memory_order_release);
    state.WarnDisconnected(result);
  } catch (...) {
    try {
      state.WarnBusiness("mqtt_connect_callback", "MQTT连接回调异常");
    } catch (...) {
    }
  }
}

void MqttClient::HandleDisconnect(struct mosquitto*, void* context,
                                  int result) noexcept {
  auto& state = *static_cast<CallbackState*>(context);
  try {
    CallbackGuard callback_guard{state};
    state.connected.store(false, std::memory_order_release);
    std::vector<PublishCompletion> completions;
    PublishCompletionHandler handler;
    {
      std::lock_guard lock{state.mutex};
      handler = state.publish_handler;
      completions.reserve(state.token_to_mid.size());
      for (const auto& [token, mid] : state.token_to_mid) {
        static_cast<void>(mid);
        completions.push_back(PublishCompletion{
            .token = token, .result = std::unexpected("MQTT连接已断开")});
      }
      state.token_to_mid.clear();
      state.mid_to_token.clear();
      state.untracked_completion_mids.clear();
    }
    if (handler) {
      for (auto& completion : completions) handler(std::move(completion));
    }
    if (result != MOSQ_ERR_SUCCESS) state.WarnDisconnected(result);
  } catch (...) {
  }
}

void MqttClient::HandlePublish(struct mosquitto*, void* context,
                               int mid) noexcept {
  auto& state = *static_cast<CallbackState*>(context);
  try {
    CallbackGuard callback_guard{state};
    PublishCompletionHandler handler;
    std::optional<std::uint64_t> token;
    bool unknown_mid = false;
    {
      std::lock_guard lock{state.mutex};
      const auto found = state.mid_to_token.find(mid);
      if (found == state.mid_to_token.end()) {
        if (state.untracked_completion_mids.erase(mid) == 0) {
          unknown_mid = true;
        }
      } else {
        token = found->second;
        state.token_to_mid.erase(*token);
        state.mid_to_token.erase(found);
        handler = state.publish_handler;
      }
    }
    if (unknown_mid) {
      state.WarnBusiness("mqtt_unknown_publish_mid",
                         "MQTT收到未知或重复的发布完成MID");
      return;
    }
    if (handler) handler(PublishCompletion{.token = *token, .result = {}});
  } catch (...) {
    try {
      state.WarnBusiness("mqtt_publish_callback", "MQTT发布完成回调异常");
    } catch (...) {
    }
  }
}

void MqttClient::HandleLog(struct mosquitto*, void* context, int level,
                           const char* message) noexcept {
  static_cast<void>(message);
  auto& state = *static_cast<CallbackState*>(context);
  try {
    CallbackGuard callback_guard{state};
    if ((level & MOSQ_LOG_ERR) != 0) {
      state.WarnLibraryEvent("mqtt_library_error", "MQTT库报告错误事件");
    } else if ((level & MOSQ_LOG_WARNING) != 0) {
      state.WarnLibraryEvent("mqtt_library_warning", "MQTT库报告警告事件");
    }
  } catch (...) {
  }
}

void MqttClient::HandleMessage(
    struct mosquitto*, void* context,
    const struct mosquitto_message* message) noexcept {
  const auto received_at = std::chrono::system_clock::now();
  auto& state = *static_cast<CallbackState*>(context);
  try {
    CallbackGuard callback_guard{state};
    state.Dispatch(message, received_at);
  } catch (...) {
    try {
      state.WarnBusiness("mqtt_message_callback", "MQTT消息回调异常");
    } catch (...) {
    }
  }
}

void MqttClient::WarnDisconnected(int result) {
  callback_state_->WarnDisconnected(result);
}

void MqttClient::RetryConnection() {
  auto retry_delay = config_.reconnect_delay;
  while (true) {
    std::unique_lock retry_lock{retry_mutex_};
    if (cns_runtime_wait_interruptibly(&retry_changed_, &retry_lock, retry_delay,
                                       &retry_stop_requested_)) {
      retry_lock.unlock();
      FinishRetry();
      return;
    }
    retry_lock.unlock();

    const int connect_result = mosquitto_connect_async(
        client_, config_.host.c_str(), static_cast<int>(config_.port),
        static_cast<int>(config_.keepalive.count()));
    if (connect_result == MOSQ_ERR_EAI) {
      WarnDisconnected(connect_result);
      retry_delay = std::min(retry_delay * 2, config_.reconnect_delay_max);
      continue;
    }
    if (connect_result != MOSQ_ERR_SUCCESS && connect_result != MOSQ_ERR_ERRNO) {
      const std::string error =
          MosquittoError("重试MQTT异步连接", connect_result);
      callback_state_->Warn(error);
      FinishRetry(error);
      return;
    }
    if (connect_result == MOSQ_ERR_ERRNO) WarnDisconnected(connect_result);

    bool stop_requested = false;
    {
      std::lock_guard lock{retry_mutex_};
      stop_requested = retry_stop_requested_;
    }
    if (stop_requested) {
      FinishRetry();
      return;
    }
    const int loop_result = mosquitto_loop_start(client_);
    if (loop_result != MOSQ_ERR_SUCCESS) {
      const std::string error = MosquittoError("启动MQTT网络线程", loop_result);
      callback_state_->Warn(error);
      static_cast<void>(mosquitto_disconnect(client_));
      FinishRetry(error);
      return;
    }
    loop_started_.store(true, std::memory_order_release);
    FinishRetry();
    return;
  }
}

void MqttClient::FinishRetry(std::string error) {
  std::lock_guard lock{retry_mutex_};
  retry_error_ = std::move(error);
  retry_state_ = retry_error_.empty() ? RetryState::kIdle : RetryState::kFailed;
  retry_changed_.notify_all();
}

}  // namespace cns::mqtt
