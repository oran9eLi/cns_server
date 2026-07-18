// 本文件声明 libmosquitto 最小连接、自动重连与生命周期封装。
#pragma once

#include <atomic>
#include <expected>
#include <memory>
#include <mutex>
#include <string>

#include "core/config/app_config.hpp"
#include "core/logging/logger.hpp"
#include "core/runtime/rate_limiter.hpp"

struct mosquitto;

namespace cns::mqtt {

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

 private:
  MqttClient(config::MqttConfig config, logging::Logger& logger);

  static void HandleConnect(struct mosquitto*, void* context, int result);
  static void HandleDisconnect(struct mosquitto*, void* context, int result);
  void WarnDisconnected(int result);

  config::MqttConfig config_;
  logging::Logger& logger_;
  runtime::RateLimiter warning_limiter_;
  struct mosquitto* client_ = nullptr;
  bool library_acquired_ = false;
  bool loop_started_ = false;
  mutable std::mutex lifecycle_mutex_;
  std::atomic_bool connected_{false};
};

}  // namespace cns::mqtt
