// 本文件声明路由服务配置的强类型结构与严格 JSON 加载接口。
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

#include "core/logging/logger.hpp"

namespace cns::config {

struct DatabaseConfig {
  std::string host;
  std::uint16_t port;
  std::string name;
  std::string user;
  std::string password;
  std::chrono::seconds connect_timeout;
  std::chrono::seconds reconnect_interval{5};
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
  std::size_t max_payload_bytes{262144};
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

enum class FixedSourceKind { kHostApp, kControlCenter };

struct FixedSourceConfig {
  std::string source_id;
  FixedSourceKind source_kind;

  bool operator==(const FixedSourceConfig&) const = default;
};

struct CommandConfig {
  std::chrono::seconds config_timeout{15};
  std::chrono::days terminal_retention{30};
  std::chrono::seconds cleanup_interval{3600};
  std::size_t cleanup_batch_size{100};
  std::size_t max_inflight_commands{256};
  std::vector<FixedSourceConfig> fixed_sources;
};

struct AppConfig {
  DatabaseConfig database;
  MqttConfig mqtt;
  LoggingConfig logging;
  QueueConfig queues;
  DeviceStateConfig device_state;
  CommandConfig command{};
};

std::expected<AppConfig, std::string> LoadAppConfig(
    const std::filesystem::path& path);

}  // namespace cns::config
