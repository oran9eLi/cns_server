// 本文件负责验证严格 JSON 配置的完整解析、字段约束与敏感信息保护。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "core/config/app_config.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <tuple>
#include <utility>

namespace {

constexpr auto kTestPassword = "测试数据库密码-不得出现在错误中";

std::string ValidJson() {
  return R"({
  "database": {
    "host": "127.0.0.1", "port": 5432, "name": "cns", "user": "cns_route",
    "password": ")" + std::string{kTestPassword} + R"(", "connect_timeout_seconds": 5,
    "reconnect_interval_seconds": 5
  },
  "mqtt": {
    "host": "127.0.0.1", "port": 1883, "keepalive_seconds": 60,
    "client_id": "cns-route-service", "username": "", "password": "",
    "reconnect_delay_seconds": 1, "reconnect_delay_max_seconds": 30,
    "topic_namespace": "cns_rpi", "max_payload_bytes": 262144
  },
  "logging": {"level": "info"},
  "queues": {"mqtt_inbound_capacity": 256},
  "device_state": {"telemetry_flush_interval_seconds": 5, "offline_timeout_seconds": 120},
  "command": {
    "config_timeout_seconds": 15,
    "terminal_retention_days": 30,
    "cleanup_interval_seconds": 3600,
    "cleanup_batch_size": 100,
    "max_inflight_commands": 256,
    "fixed_sources": [
      {"source_id": "hardware-console", "source_kind": "host_app"},
      {"source_id": "main-control", "source_kind": "control_center"},
      {"source_id": "web-console", "source_kind": "host_app"}
    ]
  }
})";
}

std::filesystem::path WriteConfig(const std::string& json) {
  static std::size_t sequence = 0;
  const auto path = std::filesystem::temp_directory_path() /
                    ("route_service_config_test_" + std::to_string(++sequence) + ".json");
  std::ofstream output(path);
  output << json;
  return path;
}

void CheckRejected(std::string json, const std::string& path) {
  const auto file = WriteConfig(json);
  const auto result = cns::config::LoadAppConfig(file);
  std::filesystem::remove(file);
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().find(path) != std::string::npos);
  CHECK(result.error().find(kTestPassword) == std::string::npos);
}

void Replace(std::string& text, const std::string& from, const std::string& to) {
  const auto position = text.find(from);
  REQUIRE(position != std::string::npos);
  text.replace(position, from.size(), to);
}

}  // namespace

TEST_CASE("完整配置准确解析为强类型结构") {
  const auto file = WriteConfig(ValidJson());
  const auto result = cns::config::LoadAppConfig(file);
  std::filesystem::remove(file);
  REQUIRE(result.has_value());
  CHECK(result->database.host == "127.0.0.1");
  CHECK(result->database.port == 5432);
  CHECK(result->database.name == "cns");
  CHECK(result->database.user == "cns_route");
  CHECK(result->database.password == kTestPassword);
  CHECK(result->database.connect_timeout == std::chrono::seconds{5});
  CHECK(result->database.reconnect_interval == std::chrono::seconds{5});
  CHECK(result->mqtt.host == "127.0.0.1");
  CHECK(result->mqtt.port == 1883);
  CHECK(result->mqtt.keepalive == std::chrono::seconds{60});
  CHECK(result->mqtt.client_id == "cns-route-service");
  CHECK(result->mqtt.username.empty());
  CHECK(result->mqtt.password.empty());
  CHECK(result->mqtt.reconnect_delay == std::chrono::seconds{1});
  CHECK(result->mqtt.reconnect_delay_max == std::chrono::seconds{30});
  CHECK(result->mqtt.topic_namespace == "cns_rpi");
  CHECK(result->mqtt.max_payload_bytes == 262144);
  CHECK(result->logging.level == cns::logging::Level::kInfo);
  CHECK(result->queues.mqtt_inbound_capacity == 256);
  CHECK(result->device_state.telemetry_flush_interval == std::chrono::seconds{5});
  CHECK(result->device_state.offline_timeout == std::chrono::seconds{120});
  CHECK(result->command.config_timeout == std::chrono::seconds{15});
  CHECK(result->command.terminal_retention == std::chrono::days{30});
  CHECK(result->command.cleanup_interval == std::chrono::seconds{3600});
  CHECK(result->command.cleanup_batch_size == 100);
  CHECK(result->command.max_inflight_commands == 256);
  REQUIRE(result->command.fixed_sources.size() == 3);
  CHECK(result->command.fixed_sources[0].source_id == "hardware-console");
  CHECK(result->command.fixed_sources[0].source_kind ==
        cns::config::FixedSourceKind::kHostApp);
  CHECK(result->command.fixed_sources[1].source_kind ==
        cns::config::FixedSourceKind::kControlCenter);
}

TEST_CASE("命令配置拒绝重复来源与非法来源") {
  auto duplicate = ValidJson();
  Replace(duplicate, "main-control", "hardware-console");
  CheckRejected(duplicate, "command.fixed_sources[1].source_id");

  auto device = ValidJson();
  Replace(device, "control_center", "device");
  CheckRejected(device, "command.fixed_sources[1].source_kind");

  auto invalid_id = ValidJson();
  Replace(invalid_id, "hardware-console", "hardware/console");
  CheckRejected(invalid_id, "command.fixed_sources[0].source_id");
}

TEST_CASE("命令配置严格校验未知字段和数值边界") {
  const auto check = [](const std::string& token, const std::string& replacement,
                        const std::string& path) {
    auto json = ValidJson();
    Replace(json, token, replacement);
    CheckRejected(json, path);
  };
  for (const auto& [token, low, high, path] :
       std::initializer_list<std::tuple<std::string, std::string, std::string,
                                        std::string>>{
           {"config_timeout_seconds\": 15", "config_timeout_seconds\": 0",
            "config_timeout_seconds\": 301", "command.config_timeout_seconds"},
           {"terminal_retention_days\": 30", "terminal_retention_days\": 0",
            "terminal_retention_days\": 3651", "command.terminal_retention_days"},
           {"cleanup_interval_seconds\": 3600", "cleanup_interval_seconds\": 59",
            "cleanup_interval_seconds\": 86401", "command.cleanup_interval_seconds"},
           {"cleanup_batch_size\": 100", "cleanup_batch_size\": 0",
            "cleanup_batch_size\": 1001", "command.cleanup_batch_size"},
           {"max_inflight_commands\": 256", "max_inflight_commands\": 0",
            "max_inflight_commands\": 4097", "command.max_inflight_commands"}}) {
    check(token, low, path);
    check(token, high, path);
  }
  check("cleanup_batch_size\": 100", "cleanup_batch_size\": true",
        "command.cleanup_batch_size");
  check("max_inflight_commands\": 256",
        "max_inflight_commands\": 256, \"unknown\": 1", "command.unknown");
}

TEST_CASE("新增配置成员具有兼容旧调用点的安全默认值") {
  const cns::config::DatabaseConfig database{};
  const cns::config::MqttConfig mqtt{};
  CHECK(database.reconnect_interval == std::chrono::seconds{5});
  CHECK(mqtt.max_payload_bytes == 262144);

  const cns::config::DatabaseConfig compatible_database{
      "host", 5432, "name", "user", "password", std::chrono::seconds{5}};
  const cns::config::MqttConfig compatible_mqtt{
      "host", 1883, std::chrono::seconds{60}, "client", "", "",
      std::chrono::seconds{1}, std::chrono::seconds{30}};
  CHECK(compatible_database.reconnect_interval == std::chrono::seconds{5});
  CHECK(compatible_mqtt.max_payload_bytes == 262144);
}

TEST_CASE("新增配置字段均为必填") {
  for (const auto& [from, to, path] : std::initializer_list<std::tuple<std::string, std::string, std::string>>{
           {R"(, "connect_timeout_seconds": 5,
    "reconnect_interval_seconds": 5)", R"(, "connect_timeout_seconds": 5)", "database.reconnect_interval_seconds"},
           {R"(,
    "topic_namespace": "cns_rpi")", "", "mqtt.topic_namespace"},
           {R"(, "max_payload_bytes": 262144)", "", "mqtt.max_payload_bytes"},
           {R"(,
  "device_state": {"telemetry_flush_interval_seconds": 5, "offline_timeout_seconds": 120})", "", "device_state"}}) {
    auto json = ValidJson(); Replace(json, from, to); CheckRejected(json, path);
  }
}

TEST_CASE("device_state 严格拒绝缺失未知与错误类型") {
  for (const auto& [from, to, path] : std::initializer_list<std::tuple<std::string, std::string, std::string>>{
           {R"("telemetry_flush_interval_seconds": 5, )", "", "device_state.telemetry_flush_interval_seconds"},
           {R"(, "offline_timeout_seconds": 120)", "", "device_state.offline_timeout_seconds"},
           {R"("offline_timeout_seconds": 120)", R"("unknown": 1, "offline_timeout_seconds": 120)", "device_state.unknown"},
           {R"("telemetry_flush_interval_seconds": 5)", R"("telemetry_flush_interval_seconds": "5")", "device_state.telemetry_flush_interval_seconds"},
           {R"("offline_timeout_seconds": 120)", R"("offline_timeout_seconds": "120")", "device_state.offline_timeout_seconds"}}) {
    auto json = ValidJson(); Replace(json, from, to); CheckRejected(json, path);
  }
}

TEST_CASE("新增整数配置接受完整合法范围端点") {
  for (const auto& [token, replacement] :
       std::initializer_list<std::pair<std::string, std::string>>{
           {"reconnect_interval_seconds\": 5", "reconnect_interval_seconds\": 1"},
           {"reconnect_interval_seconds\": 5", "reconnect_interval_seconds\": 300"},
           {"max_payload_bytes\": 262144", "max_payload_bytes\": 1024"},
           {"max_payload_bytes\": 262144", "max_payload_bytes\": 1048576"},
           {"telemetry_flush_interval_seconds\": 5", "telemetry_flush_interval_seconds\": 1"},
           {"telemetry_flush_interval_seconds\": 5", "telemetry_flush_interval_seconds\": 60"},
           {"offline_timeout_seconds\": 120", "offline_timeout_seconds\": 61"},
           {"offline_timeout_seconds\": 120", "offline_timeout_seconds\": 86400"}}) {
    auto json = ValidJson();
    Replace(json, token, replacement);
    const auto file = WriteConfig(json);
    const auto result = cns::config::LoadAppConfig(file);
    std::filesystem::remove(file);
    CHECK(result.has_value());
  }
}

TEST_CASE("新增整数配置执行类型与边界校验") {
  const auto check = [](const std::string& token, const std::string& replacement,
                        const std::string& path) {
    auto json = ValidJson(); Replace(json, token, replacement); CheckRejected(json, path);
  };
  check("reconnect_interval_seconds\": 5", "reconnect_interval_seconds\": \"5\"", "database.reconnect_interval_seconds");
  for (const auto value : {"0", "301"}) check("reconnect_interval_seconds\": 5", "reconnect_interval_seconds\": " + std::string{value}, "database.reconnect_interval_seconds");
  check("max_payload_bytes\": 262144", "max_payload_bytes\": true", "mqtt.max_payload_bytes");
  for (const auto value : {"1023", "1048577"}) check("max_payload_bytes\": 262144", "max_payload_bytes\": " + std::string{value}, "mqtt.max_payload_bytes");
  for (const auto value : {"0", "61"}) check("telemetry_flush_interval_seconds\": 5", "telemetry_flush_interval_seconds\": " + std::string{value}, "device_state.telemetry_flush_interval_seconds");
  for (const auto value : {"60", "86401"}) check("offline_timeout_seconds\": 120", "offline_timeout_seconds\": " + std::string{value}, "device_state.offline_timeout_seconds");
}

TEST_CASE("MQTT namespace 必须是单个非空合法段") {
  for (const auto value : {"", "a/b", "a+", "a#", "a b", "a\\tb"}) {
    auto json = ValidJson(); Replace(json, "cns_rpi", value); CheckRejected(json, "mqtt.topic_namespace");
  }
  auto json = ValidJson(); Replace(json, R"("topic_namespace": "cns_rpi")", R"("topic_namespace": true)");
  CheckRejected(json, "mqtt.topic_namespace");
  json = ValidJson();
  Replace(json, "cns_rpi", R"(cns\u0000rpi)");
  CheckRejected(json, "mqtt.topic_namespace");
}

TEST_CASE("新增配置对象拒绝未知字段") {
  auto json = ValidJson();
  Replace(json, R"("max_payload_bytes": 262144)", R"("max_payload_bytes": 262144, "unknown": 1)");
  CheckRejected(json, "mqtt.unknown");
}

TEST_CASE("离线超时必须严格大于 MQTT keepalive") {
  auto json = ValidJson();
  Replace(json, "offline_timeout_seconds\": 120", "offline_timeout_seconds\": 60");
  CheckRejected(json, "device_state.offline_timeout_seconds");
}

TEST_CASE("未知顶层字段被拒绝") {
  auto json = ValidJson();
  Replace(json, R"("queues":)", R"("unexpected": 1, "queues":)");
  CheckRejected(json, "unexpected");
}

TEST_CASE("未知嵌套字段被拒绝") {
  auto json = ValidJson();
  Replace(json, R"("host": "127.0.0.1", "port": 5432)",
          R"("host": "127.0.0.1", "unknown": true, "port": 5432)");
  CheckRejected(json, "database.unknown");
}

TEST_CASE("缺失字段被拒绝") {
  auto json = ValidJson();
  Replace(json, R"("name": "cns", )", "");
  CheckRejected(json, "database.name");
}

TEST_CASE("错误类型被拒绝") {
  auto json = ValidJson();
  Replace(json, R"("port": 5432)", R"("port": "5432")");
  CheckRejected(json, "database.port");
}

TEST_CASE("端口必须处于有效范围") {
  for (const auto value : {"0", "65536"}) {
    auto json = ValidJson();
    Replace(json, "\"port\": 5432", "\"port\": " + std::string{value});
    CheckRejected(json, "database.port");
  }
}

TEST_CASE("连接超时必须大于零") {
  auto json = ValidJson();
  Replace(json, "connect_timeout_seconds\": 5", "connect_timeout_seconds\": 0");
  CheckRejected(json, "database.connect_timeout_seconds");
}

TEST_CASE("MQTT keepalive 必须大于零") {
  auto json = ValidJson();
  Replace(json, "keepalive_seconds\": 60", "keepalive_seconds\": 0");
  CheckRejected(json, "mqtt.keepalive_seconds");
}

TEST_CASE("MQTT 重连初值不得大于最大值") {
  auto json = ValidJson();
  Replace(json, "reconnect_delay_seconds\": 1", "reconnect_delay_seconds\": 31");
  CheckRejected(json, "mqtt.reconnect_delay_seconds");
}

TEST_CASE("数据库密码不能为空") {
  auto json = ValidJson();
  Replace(json, kTestPassword, "");
  CheckRejected(json, "database.password");
}

TEST_CASE("MQTT client_id 不能为空") {
  auto json = ValidJson();
  Replace(json, "cns-route-service", "");
  CheckRejected(json, "mqtt.client_id");
}

TEST_CASE("队列容量必须大于零") {
  auto json = ValidJson();
  Replace(json, "mqtt_inbound_capacity\": 256", "mqtt_inbound_capacity\": 0");
  CheckRejected(json, "queues.mqtt_inbound_capacity");
}

TEST_CASE("日志等级只接受约定值") {
  auto json = ValidJson();
  Replace(json, R"("level": "info")", R"("level": "trace")");
  CheckRejected(json, "logging.level");
}

TEST_CASE("MQTT 匿名配置不允许仅提供密码") {
  auto json = ValidJson();
  Replace(json, R"("username": "", "password": "")",
          R"("username": "", "password": "secret")");
  CheckRejected(json, "mqtt.password");
}
