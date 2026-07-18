// 本文件实现 libmosquitto 全局库租约、异步连接、自动重连和安全停止。
#include "adapters/mqtt/mqtt_client.hpp"

#include <mosquitto.h>

#include <chrono>
#include <cstddef>
#include <string>
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

MqttClient::MqttClient(config::MqttConfig config, logging::Logger& logger)
    : config_(std::move(config)),
      logger_(logger),
      warning_limiter_(std::chrono::seconds{30}) {}

std::expected<std::unique_ptr<MqttClient>, std::string> MqttClient::Create(
    const config::MqttConfig& config, logging::Logger& logger) {
  auto instance = std::unique_ptr<MqttClient>{new MqttClient(config, logger)};
  const int library_result = AcquireLibrary();
  if (library_result != MOSQ_ERR_SUCCESS) {
    return std::unexpected(MosquittoError("初始化MQTT库", library_result));
  }
  instance->library_acquired_ = true;

  instance->client_ =
      mosquitto_new(config.client_id.c_str(), true, instance.get());
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
  if (loop_started_) return {};

  const int connect_result = mosquitto_connect_async(
      client_, config_.host.c_str(), static_cast<int>(config_.port),
      static_cast<int>(config_.keepalive.count()));
  const bool initial_network_failure =
      connect_result == MOSQ_ERR_ERRNO || connect_result == MOSQ_ERR_EAI;
  if (connect_result != MOSQ_ERR_SUCCESS && !initial_network_failure) {
    return std::unexpected(MosquittoError("启动MQTT异步连接", connect_result));
  }
  if (initial_network_failure) WarnDisconnected(connect_result);

  const int loop_result = mosquitto_loop_start(client_);
  if (loop_result != MOSQ_ERR_SUCCESS) {
    static_cast<void>(mosquitto_disconnect(client_));
    return std::unexpected(MosquittoError("启动MQTT网络线程", loop_result));
  }
  loop_started_ = true;
  return {};
}

void MqttClient::Stop() {
  std::lock_guard lock{lifecycle_mutex_};
  if (!loop_started_) {
    connected_.store(false, std::memory_order_release);
    return;
  }

  static_cast<void>(mosquitto_loop_stop(client_, true));
  static_cast<void>(mosquitto_disconnect(client_));
  loop_started_ = false;
  connected_.store(false, std::memory_order_release);
}

bool MqttClient::IsConnected() const noexcept {
  return connected_.load(std::memory_order_acquire);
}

void MqttClient::HandleConnect(struct mosquitto*, void* context, int result) {
  auto& self = *static_cast<MqttClient*>(context);
  if (result == 0) {
    self.connected_.store(true, std::memory_order_release);
    self.logger_.Info("MQTT连接已恢复");
    return;
  }
  self.connected_.store(false, std::memory_order_release);
  self.WarnDisconnected(result);
}

void MqttClient::HandleDisconnect(struct mosquitto*, void* context, int result) {
  auto& self = *static_cast<MqttClient*>(context);
  self.connected_.store(false, std::memory_order_release);
  if (result != MOSQ_ERR_SUCCESS) self.WarnDisconnected(result);
}

void MqttClient::HandleLog(struct mosquitto*, void* context, int level,
                           const char* message) {
  static_cast<void>(message);
  auto& self = *static_cast<MqttClient*>(context);
  self.logger_.Debug("MQTT库事件，级别=" + std::to_string(level));
}

void MqttClient::WarnDisconnected(int result) {
  if (!warning_limiter_.ShouldEmit("mqtt_disconnect",
                                   runtime::RateLimiter::Clock::now())) {
    return;
  }
  logger_.Warn("MQTT连接中断，将自动重连，错误码=" + std::to_string(result));
}

}  // namespace cns::mqtt
