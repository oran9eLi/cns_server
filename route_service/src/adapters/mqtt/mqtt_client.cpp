// 本文件实现 libmosquitto 全局库租约、异步连接、自动重连和安全停止。
#include "adapters/mqtt/mqtt_client.hpp"

#include <mosquitto.h>

#include <chrono>
#include <cstddef>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace cns::mqtt {
namespace {

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
        library_logs(std::chrono::seconds{30}) {}

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
    connected.store(true, std::memory_order_release);
    std::lock_guard lock{mutex};
    if (logger != nullptr) logger->Info("MQTT连接已恢复");
  }

  std::mutex mutex;
  logging::Logger* logger;
  runtime::RateLimiter disconnect_warnings;
  runtime::RateLimiter library_logs;
  std::atomic_bool connected{false};
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
  if (loop_started_.load(std::memory_order_acquire) || retry_thread_.joinable()) {
    return {};
  }
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
    try {
      retry_thread_ = std::thread(&MqttClient::RetryConnection, this);
    } catch (const std::system_error& error) {
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
  std::lock_guard lock{lifecycle_mutex_};
  {
    std::lock_guard retry_lock{retry_mutex_};
    retry_stop_requested_ = true;
  }
  retry_changed_.notify_all();
  if (retry_thread_.joinable()) retry_thread_.join();

  if (!loop_started_.load(std::memory_order_acquire)) {
    callback_state_->connected.store(false, std::memory_order_release);
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
}

bool MqttClient::IsConnected() const noexcept {
  return callback_state_->connected.load(std::memory_order_acquire);
}

void MqttClient::HandleConnect(struct mosquitto*, void* context, int result) {
  auto& state = *static_cast<CallbackState*>(context);
  if (result == 0) {
    state.Connected();
    return;
  }
  state.connected.store(false, std::memory_order_release);
  state.WarnDisconnected(result);
}

void MqttClient::HandleDisconnect(struct mosquitto*, void* context, int result) {
  auto& state = *static_cast<CallbackState*>(context);
  state.connected.store(false, std::memory_order_release);
  if (result != MOSQ_ERR_SUCCESS) state.WarnDisconnected(result);
}

void MqttClient::HandleLog(struct mosquitto*, void* context, int level,
                           const char* message) {
  static_cast<void>(message);
  auto& state = *static_cast<CallbackState*>(context);
  if ((level & MOSQ_LOG_ERR) != 0) {
    state.WarnLibraryEvent("mqtt_library_error", "MQTT库报告错误事件");
  } else if ((level & MOSQ_LOG_WARNING) != 0) {
    state.WarnLibraryEvent("mqtt_library_warning", "MQTT库报告警告事件");
  }
}

void MqttClient::WarnDisconnected(int result) {
  callback_state_->WarnDisconnected(result);
}

void MqttClient::RetryConnection() {
  while (true) {
    {
      std::lock_guard lock{retry_mutex_};
      if (retry_stop_requested_) return;
    }

    const int connect_result = mosquitto_connect_async(
        client_, config_.host.c_str(), static_cast<int>(config_.port),
        static_cast<int>(config_.keepalive.count()));
    if (connect_result == MOSQ_ERR_EAI) {
      WarnDisconnected(connect_result);
      std::unique_lock lock{retry_mutex_};
      if (retry_changed_.wait_for(lock, config_.reconnect_delay,
                                  [this] { return retry_stop_requested_; })) {
        return;
      }
      continue;
    }
    if (connect_result != MOSQ_ERR_SUCCESS && connect_result != MOSQ_ERR_ERRNO) {
      callback_state_->Warn(
          MosquittoError("重试MQTT异步连接", connect_result));
      return;
    }
    if (connect_result == MOSQ_ERR_ERRNO) WarnDisconnected(connect_result);

    {
      std::lock_guard lock{retry_mutex_};
      if (retry_stop_requested_) return;
    }
    const int loop_result = mosquitto_loop_start(client_);
    if (loop_result != MOSQ_ERR_SUCCESS) {
      callback_state_->Warn(
          MosquittoError("启动MQTT网络线程", loop_result));
      static_cast<void>(mosquitto_disconnect(client_));
      return;
    }
    loop_started_.store(true, std::memory_order_release);
    return;
  }
}

}  // namespace cns::mqtt
