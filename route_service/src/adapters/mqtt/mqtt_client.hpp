// 本文件声明 libmosquitto 最小连接、自动重连与生命周期封装。
#pragma once

#include <atomic>
#include <condition_variable>
#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "core/config/app_config.hpp"
#include "core/logging/logger.hpp"
#include "core/runtime/rate_limiter.hpp"

struct mosquitto;

namespace cns::mqtt {

/** 表示 MQTT 当前可供进程编排安全观察的运行状态。 */
enum class RuntimeStatus { kDisconnectedOrRetrying, kConnected, kTerminalFailure };

class MqttClient {
 public:
  static std::expected<std::unique_ptr<MqttClient>, std::string> Create(
      const config::MqttConfig& config, logging::Logger& logger);

  ~MqttClient();
  MqttClient(const MqttClient&) = delete;
  MqttClient& operator=(const MqttClient&) = delete;

  std::expected<void, std::string> Start();
  void Stop();
  bool IsConnected() const noexcept;
  RuntimeStatus GetRuntimeStatus() const noexcept;

 private:
  struct CallbackState;
  enum class RetryState { kIdle, kRunning, kFailed };

  MqttClient(config::MqttConfig config, logging::Logger& logger);

  static void HandleConnect(struct mosquitto*, void* context, int result);
  static void HandleDisconnect(struct mosquitto*, void* context, int result);
  static void HandleLog(struct mosquitto*, void* context, int level,
                        const char* message);
  void WarnDisconnected(int result);
  void RetryConnection();
  void FinishRetry(std::string error = {});

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
};

}  // namespace cns::mqtt
