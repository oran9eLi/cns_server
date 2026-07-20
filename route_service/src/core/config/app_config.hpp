// 本文件声明路由服务配置的强类型结构与严格 JSON 加载接口。
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>

#include "core/logging/logger.hpp"

namespace cns::config {

struct DatabaseConfig {
  std::string host;
  std::uint16_t port;
  std::string name;
  std::string user;
  std::string password;
  std::chrono::seconds connect_timeout;
  std::chrono::seconds reconnect_interval{};
};

struct MqttConfig {
  std::string host;
  std::uint16_t port;
  std::chrono::seconds keepalive;
  std::string client_id;
  std::string username;
  std::string password;
  std::chrono::seconds reconnect_delay;
  std::chrono::seconds reconnect_delay_max;
  std::string topic_namespace{};
  std::size_t max_payload_bytes{};
};

struct LoggingConfig {
  logging::Level level;
};

struct QueueConfig {
  std::size_t mqtt_inbound_capacity;
};

struct DeviceStateConfig {
  std::chrono::seconds telemetry_flush_interval;
  std::chrono::seconds offline_timeout;
};

struct AppConfig {
  DatabaseConfig database;
  MqttConfig mqtt;
  LoggingConfig logging;
  QueueConfig queues;
  DeviceStateConfig device_state;
};

std::expected<AppConfig, std::string> LoadAppConfig(
    const std::filesystem::path& path);

}  // namespace cns::config
