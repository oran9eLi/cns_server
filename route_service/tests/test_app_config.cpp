// 本文件负责验证严格 JSON 配置的完整解析、字段约束与敏感信息保护。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "core/config/app_config.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

constexpr auto kTestPassword = "测试数据库密码-不得出现在错误中";

std::string ValidJson() {
  return R"({
  "database": {
    "host": "127.0.0.1", "port": 5432, "name": "cns", "user": "cns_route",
    "password": ")" + std::string{kTestPassword} + R"(", "connect_timeout_seconds": 5
  },
  "mqtt": {
    "host": "127.0.0.1", "port": 1883, "keepalive_seconds": 60,
    "client_id": "cns-route-service", "username": "", "password": "",
    "reconnect_delay_seconds": 1, "reconnect_delay_max_seconds": 30
  },
  "logging": {"level": "info"},
  "queues": {"mqtt_inbound_capacity": 256}
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
  CHECK(result->mqtt.host == "127.0.0.1");
  CHECK(result->mqtt.port == 1883);
  CHECK(result->mqtt.keepalive == std::chrono::seconds{60});
  CHECK(result->mqtt.client_id == "cns-route-service");
  CHECK(result->mqtt.username.empty());
  CHECK(result->mqtt.password.empty());
  CHECK(result->mqtt.reconnect_delay == std::chrono::seconds{1});
  CHECK(result->mqtt.reconnect_delay_max == std::chrono::seconds{30});
  CHECK(result->logging.level == cns::logging::Level::kInfo);
  CHECK(result->queues.mqtt_inbound_capacity == 256);
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
