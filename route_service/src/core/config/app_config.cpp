// 本文件负责读取配置文件并将严格校验后的 JSON 转换为强类型配置。
#include "core/config/app_config.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string_view>

namespace cns::config {
namespace {

using Json = nlohmann::json;
using namespace std::string_view_literals;

std::unexpected<std::string> Error(std::string_view path,
                                   std::string_view message) {
  return std::unexpected(std::string{path} + "：" + std::string{message});
}

template <std::size_t Size>
std::expected<const Json*, std::string> ReadObject(
    const Json& parent, std::string_view key, std::string_view path,
    const std::array<std::string_view, Size>& allowed) {
  if (!parent.contains(key)) {
    return Error(path, "缺少必填字段");
  }
  const auto& object = parent.at(key);
  if (!object.is_object()) {
    return Error(path, "必须是对象");
  }
  for (const auto& [field, unused] : object.items()) {
    (void)unused;
    if (std::ranges::find(allowed, field) == allowed.end()) {
      return Error(std::string{path} + "." + field, "未知字段");
    }
  }
  return &object;
}

std::expected<std::string, std::string> ReadString(const Json& object,
                                                   std::string_view key,
                                                   std::string_view path) {
  if (!object.contains(key)) {
    return Error(path, "缺少必填字段");
  }
  if (!object.at(key).is_string()) {
    return Error(path, "必须是字符串");
  }
  return object.at(key).get<std::string>();
}

std::expected<std::uint64_t, std::string> ReadInteger(
    const Json& object, std::string_view key, std::string_view path,
    std::uint64_t minimum, std::uint64_t maximum) {
  if (!object.contains(key)) {
    return Error(path, "缺少必填字段");
  }
  const auto& value = object.at(key);
  if (!value.is_number_integer() && !value.is_number_unsigned()) {
    return Error(path, "必须是整数");
  }
  if (value.is_number_integer() && value.get<std::int64_t>() < 0) {
    return Error(path, "数值超出允许范围");
  }
  const auto number = value.get<std::uint64_t>();
  if (number < minimum || number > maximum) {
    return Error(path, "数值超出允许范围");
  }
  return number;
}

std::expected<void, std::string> CheckRoot(const Json& root) {
  if (!root.is_object()) {
    return Error("配置", "顶层必须是对象");
  }
  constexpr std::array allowed{"database"sv, "mqtt"sv, "logging"sv, "queues"sv,
                               "device_state"sv};
  for (const auto& [field, unused] : root.items()) {
    (void)unused;
    if (std::ranges::find(allowed, field) == allowed.end()) {
      return Error(field, "未知字段");
    }
  }
  return {};
}

std::expected<DatabaseConfig, std::string> ParseDatabase(const Json& root) {
  constexpr std::array allowed{"host"sv, "port"sv, "name"sv, "user"sv,
                               "password"sv, "connect_timeout_seconds"sv,
                               "reconnect_interval_seconds"sv};
  const auto object = ReadObject(root, "database", "database", allowed);
  if (!object) return std::unexpected(object.error());
  const auto host = ReadString(**object, "host", "database.host");
  if (!host) return std::unexpected(host.error());
  const auto port = ReadInteger(**object, "port", "database.port", 1, 65535);
  if (!port) return std::unexpected(port.error());
  const auto name = ReadString(**object, "name", "database.name");
  if (!name) return std::unexpected(name.error());
  const auto user = ReadString(**object, "user", "database.user");
  if (!user) return std::unexpected(user.error());
  const auto password = ReadString(**object, "password", "database.password");
  if (!password) return std::unexpected(password.error());
  if (password->empty()) return Error("database.password", "不能为空");
  const auto timeout = ReadInteger(**object, "connect_timeout_seconds",
                                   "database.connect_timeout_seconds", 1, 60);
  if (!timeout) return std::unexpected(timeout.error());
  const auto reconnect = ReadInteger(**object, "reconnect_interval_seconds",
                                     "database.reconnect_interval_seconds", 1, 300);
  if (!reconnect) return std::unexpected(reconnect.error());
  return DatabaseConfig{*host, static_cast<std::uint16_t>(*port), *name, *user,
                        *password, std::chrono::seconds{static_cast<long>(*timeout)},
                        std::chrono::seconds{static_cast<long>(*reconnect)}};
}

bool IsValidTopicNamespace(std::string_view value) {
  if (value.empty()) return false;
  return std::ranges::none_of(value, [](unsigned char character) {
    return character == '/' || character == '+' || character == '#' ||
           std::isspace(character) != 0;
  });
}

std::expected<MqttConfig, std::string> ParseMqtt(const Json& root) {
  constexpr std::array allowed{
      "host"sv, "port"sv, "keepalive_seconds"sv, "client_id"sv, "username"sv,
      "password"sv, "reconnect_delay_seconds"sv, "reconnect_delay_max_seconds"sv,
      "topic_namespace"sv, "max_payload_bytes"sv};
  const auto object = ReadObject(root, "mqtt", "mqtt", allowed);
  if (!object) return std::unexpected(object.error());
  const auto host = ReadString(**object, "host", "mqtt.host");
  if (!host) return std::unexpected(host.error());
  const auto port = ReadInteger(**object, "port", "mqtt.port", 1, 65535);
  if (!port) return std::unexpected(port.error());
  const auto keepalive = ReadInteger(**object, "keepalive_seconds",
                                     "mqtt.keepalive_seconds", 1, 3600);
  if (!keepalive) return std::unexpected(keepalive.error());
  const auto client_id = ReadString(**object, "client_id", "mqtt.client_id");
  if (!client_id) return std::unexpected(client_id.error());
  if (client_id->empty()) return Error("mqtt.client_id", "不能为空");
  const auto username = ReadString(**object, "username", "mqtt.username");
  if (!username) return std::unexpected(username.error());
  const auto password = ReadString(**object, "password", "mqtt.password");
  if (!password) return std::unexpected(password.error());
  if (username->empty() && !password->empty()) {
    return Error("mqtt.password", "用户名为空时密码也必须为空");
  }
  const auto delay = ReadInteger(**object, "reconnect_delay_seconds",
                                 "mqtt.reconnect_delay_seconds", 1, 3600);
  if (!delay) return std::unexpected(delay.error());
  const auto delay_max = ReadInteger(**object, "reconnect_delay_max_seconds",
                                     "mqtt.reconnect_delay_max_seconds", 1, 3600);
  if (!delay_max) return std::unexpected(delay_max.error());
  if (*delay > *delay_max) {
    return Error("mqtt.reconnect_delay_seconds", "不得大于 mqtt.reconnect_delay_max_seconds");
  }
  const auto topic_namespace = ReadString(**object, "topic_namespace", "mqtt.topic_namespace");
  if (!topic_namespace) return std::unexpected(topic_namespace.error());
  if (!IsValidTopicNamespace(*topic_namespace)) {
    return Error("mqtt.topic_namespace", "必须是单个非空合法 topic 段");
  }
  const auto max_payload = ReadInteger(**object, "max_payload_bytes",
                                       "mqtt.max_payload_bytes", 1024, 1048576);
  if (!max_payload) return std::unexpected(max_payload.error());
  return MqttConfig{*host,
                    static_cast<std::uint16_t>(*port),
                    std::chrono::seconds{static_cast<long>(*keepalive)},
                    *client_id,
                    *username,
                    *password,
                    std::chrono::seconds{static_cast<long>(*delay)},
                    std::chrono::seconds{static_cast<long>(*delay_max)},
                    *topic_namespace,
                    static_cast<std::size_t>(*max_payload)};
}

std::expected<DeviceStateConfig, std::string> ParseDeviceState(
    const Json& root, std::chrono::seconds keepalive) {
  constexpr std::array allowed{"telemetry_flush_interval_seconds"sv,
                               "offline_timeout_seconds"sv};
  const auto object = ReadObject(root, "device_state", "device_state", allowed);
  if (!object) return std::unexpected(object.error());
  const auto flush = ReadInteger(**object, "telemetry_flush_interval_seconds",
                                 "device_state.telemetry_flush_interval_seconds", 1, 60);
  if (!flush) return std::unexpected(flush.error());
  const auto offline = ReadInteger(**object, "offline_timeout_seconds",
                                   "device_state.offline_timeout_seconds", 61, 86400);
  if (!offline) return std::unexpected(offline.error());
  if (std::chrono::seconds{static_cast<long>(*offline)} <= keepalive) {
    return Error("device_state.offline_timeout_seconds",
                 "必须严格大于 mqtt.keepalive_seconds");
  }
  return DeviceStateConfig{std::chrono::seconds{static_cast<long>(*flush)},
                           std::chrono::seconds{static_cast<long>(*offline)}};
}

std::expected<LoggingConfig, std::string> ParseLogging(const Json& root) {
  constexpr std::array allowed{"level"sv};
  const auto object = ReadObject(root, "logging", "logging", allowed);
  if (!object) return std::unexpected(object.error());
  const auto level = ReadString(**object, "level", "logging.level");
  if (!level) return std::unexpected(level.error());
  const auto parsed = logging::ParseLevel(*level);
  if (!parsed) {
    return Error("logging.level", "只接受 debug、info、warn 或 error");
  }
  return LoggingConfig{*parsed};
}

std::expected<QueueConfig, std::string> ParseQueues(const Json& root) {
  constexpr std::array allowed{"mqtt_inbound_capacity"sv};
  const auto object = ReadObject(root, "queues", "queues", allowed);
  if (!object) return std::unexpected(object.error());
  const auto capacity = ReadInteger(**object, "mqtt_inbound_capacity",
                                    "queues.mqtt_inbound_capacity", 1, 65536);
  if (!capacity) return std::unexpected(capacity.error());
  return QueueConfig{static_cast<std::size_t>(*capacity)};
}

}  // namespace

std::expected<AppConfig, std::string> LoadAppConfig(
    const std::filesystem::path& path) {
  try {
    std::ifstream input(path);
    if (!input) return Error("配置文件", "无法打开");
    const std::string contents{std::istreambuf_iterator<char>{input}, {}};
    const auto root = Json::parse(contents);
    const auto root_check = CheckRoot(root);
    if (!root_check) return std::unexpected(root_check.error());
    const auto database = ParseDatabase(root);
    if (!database) return std::unexpected(database.error());
    const auto mqtt = ParseMqtt(root);
    if (!mqtt) return std::unexpected(mqtt.error());
    const auto logging = ParseLogging(root);
    if (!logging) return std::unexpected(logging.error());
    const auto queues = ParseQueues(root);
    if (!queues) return std::unexpected(queues.error());
    const auto device_state = ParseDeviceState(root, mqtt->keepalive);
    if (!device_state) return std::unexpected(device_state.error());
    return AppConfig{*database, *mqtt, *logging, *queues, *device_state};
  } catch (const Json::exception&) {
    return Error("配置文件", "JSON 格式错误");
  } catch (const std::exception&) {
    return Error("配置文件", "读取或解析失败");
  }
}

}  // namespace cns::config
