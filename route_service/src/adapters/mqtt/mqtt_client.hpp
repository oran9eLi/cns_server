// 本文件声明 libmosquitto 最小连接、自动重连与生命周期封装。
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "core/config/app_config.hpp"
#include "core/logging/logger.hpp"
#include "core/runtime/rate_limiter.hpp"

struct mosquitto;
struct mosquitto_message;

namespace cns::mqtt {

/** 表示 MQTT 当前可供进程编排安全观察的运行状态。 */
enum class RuntimeStatus { kDisconnectedOrRetrying, kConnected, kTerminalFailure };

struct InboundMessage {
  std::string topic;
  std::string payload;
  std::chrono::system_clock::time_point received_at;
};

using MessageHandler = std::function<void(InboundMessage)>;

class MqttClient {
 public:
  static std::expected<std::unique_ptr<MqttClient>, std::string> Create(
      const config::MqttConfig& config, logging::Logger& logger);

  ~MqttClient();
  MqttClient(const MqttClient&) = delete;
  MqttClient& operator=(const MqttClient&) = delete;

  std::expected<void, std::string> Start();
  /**
   * 外部生命周期线程调用时同步停止；在本客户端 C 回调内调用时仅设置
   * 异步停止请求并立即返回，外部编排应轮询 CallbackStopRequested() 后调用 Stop。
   */
  void Stop();
  /** 返回 MQTT 回调是否请求外部生命周期线程执行 Stop。 */
  bool CallbackStopRequested() const noexcept;
  bool IsConnected() const noexcept;
  RuntimeStatus GetRuntimeStatus() const noexcept;
  std::expected<void, std::string> ConfigureBusinessMessages(
      MessageHandler handler, std::size_t max_payload_bytes);
  std::expected<void, std::string> SubscribeDeviceMessages(
      std::string_view topic_namespace);
  std::expected<void, std::string> ReplayRetainedRegistrations(
      std::string_view topic_namespace);
  std::expected<void, std::string> PublishStateEvent(std::string_view topic,
                                                     std::string_view payload);

 private:
  struct CallbackState;
  struct CallbackGuard;
  enum class RetryState { kIdle, kRunning, kFailed };

  MqttClient(config::MqttConfig config, logging::Logger& logger);

  static void HandleConnect(struct mosquitto*, void* context,
                            int result) noexcept;
  static void HandleDisconnect(struct mosquitto*, void* context,
                               int result) noexcept;
  static void HandleLog(struct mosquitto*, void* context, int level,
                        const char* message) noexcept;
  static void HandleMessage(struct mosquitto*, void* context,
                            const struct mosquitto_message* message) noexcept;
  void WarnDisconnected(int result);
  void RetryConnection();
  void FinishRetry(std::string error = {});
  void StopNow();

  config::MqttConfig config_;
  std::unique_ptr<CallbackState> callback_state_;
  struct mosquitto* client_ = nullptr;
  bool library_acquired_ = false;
  std::atomic_bool loop_started_{false};
  mutable std::mutex lifecycle_mutex_;
  mutable std::mutex retry_mutex_;
  std::condition_variable retry_changed_;
  bool retry_stop_requested_ = false;
  RetryState retry_state_ = RetryState::kIdle;
  std::string retry_error_;
  std::thread retry_thread_;
  std::atomic_bool callback_stop_requested_{false};
};

}  // namespace cns::mqtt
