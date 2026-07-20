// 本文件验证 PostgreSQL 设备存储的公开接口和 SQL 安全契约。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "adapters/postgres/postgres_store.hpp"

#include <fstream>
#include <iterator>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>

namespace {

std::string StoreSource() {
  std::ifstream input{CNS_POSTGRES_STORE_SOURCE_FILE};
  REQUIRE(input.is_open());
  return {std::istreambuf_iterator<char>{input},
          std::istreambuf_iterator<char>{}};
}

TEST_CASE("设备接口提供建档请求和打开状态查询") {
  using Store = cns::postgres::PostgresStore;
  using Request = cns::postgres::ProvisionRequest;

  CHECK(std::is_same_v<decltype(&Store::LoadDevices),
                       std::expected<std::vector<cns::device::DeviceRecord>,
                                     std::string> (Store::*)()>);
  CHECK(std::is_same_v<decltype(&Store::ProvisionDevice),
                       std::expected<cns::device::DeviceRecord, std::string> (
                           Store::*)(const Request&)>);
  CHECK(std::is_same_v<decltype(&Store::WriteDeviceState),
                       std::expected<void, std::string> (Store::*)(
                           const cns::persistence::DesiredDeviceWrite&)>);
  CHECK(noexcept(std::declval<const Store&>().IsOpen()));
}

TEST_CASE("加载设备使用学校设备和最新状态的全量连接") {
  const std::string source = StoreSource();

  CHECK(source.find("FROM devices") != std::string::npos);
  CHECK(source.find("JOIN schools") != std::string::npos);
  CHECK(source.find("JOIN device_latest_states") != std::string::npos);
}

TEST_CASE("建档在单个工作事务写入四张表且不重新启用冲突来源") {
  const std::string source = StoreSource();

  CHECK(source.find("INSERT INTO schools") != std::string::npos);
  CHECK(source.find("INSERT INTO devices") != std::string::npos);
  CHECK(source.find("INSERT INTO device_latest_states") != std::string::npos);
  CHECK(source.find("INSERT INTO command_sources") != std::string::npos);
  CHECK(source.find("enabled = true") == std::string::npos);
  CHECK(source.find("DO NOTHING") != std::string::npos);
}

TEST_CASE("状态写入参数化并由数据库转换JSONB和时间") {
  const std::string source = StoreSource();

  CHECK(source.find("$6::jsonb") != std::string::npos);
  CHECK(source.find("to_timestamp($") != std::string::npos);
  CHECK(source.find("dump()") != std::string::npos);
}

TEST_CASE("显式启用时从真实数据库加载设备") {
  if (std::getenv("CNS_TEST_POSTGRES") == nullptr) {
    MESSAGE("未设置 CNS_TEST_POSTGRES，跳过真实数据库子用例");
    return;
  }
  const char* host = std::getenv("CNS_TEST_POSTGRES_HOST");
  const char* port = std::getenv("CNS_TEST_POSTGRES_PORT");
  const char* name = std::getenv("CNS_TEST_POSTGRES_DB");
  const char* user = std::getenv("CNS_TEST_POSTGRES_USER");
  const char* password = std::getenv("CNS_TEST_POSTGRES_PASSWORD");
  REQUIRE(host != nullptr);
  REQUIRE(port != nullptr);
  REQUIRE(name != nullptr);
  REQUIRE(user != nullptr);
  REQUIRE(password != nullptr);

  std::ostringstream output;
  std::ostringstream errors;
  cns::logging::Logger logger{cns::logging::Level::kError, output, errors};
  const cns::config::DatabaseConfig config{
      host, static_cast<std::uint16_t>(std::stoi(port)), name, user, password,
      std::chrono::seconds{3}};
  auto store = cns::postgres::PostgresStore::Connect(config, logger);
  REQUIRE(store.has_value());
  CHECK((*store)->IsOpen());
  CHECK((*store)->LoadDevices().has_value());
}

}  // namespace
