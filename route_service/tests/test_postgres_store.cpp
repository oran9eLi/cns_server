// 本文件验证 PostgreSQL 适配层不依赖数据库的连接参数构造契约。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "adapters/postgres/postgres_store.hpp"

#include <chrono>
#include <fstream>
#include <iterator>
#include <string>

namespace {

using cns::config::DatabaseConfig;
using cns::postgres::BuildConnectionString;
using cns::postgres::BuildSafeConnectionDescription;

TEST_CASE("连接串包含完整参数并按 libpq 规则转义") {
  const DatabaseConfig config{"db host", 5433, "cns ' route", "route user",
                              "secret ' value", std::chrono::seconds{7}};

  const std::string connection = BuildConnectionString(config);

  CHECK(connection.find("host='db host'") != std::string::npos);
  CHECK(connection.find("port='5433'") != std::string::npos);
  CHECK(connection.find("dbname='cns \\' route'") != std::string::npos);
  CHECK(connection.find("user='route user'") != std::string::npos);
  CHECK(connection.find("password='secret \\' value'") != std::string::npos);
  CHECK(connection.find("connect_timeout='7'") != std::string::npos);
}

TEST_CASE("安全连接描述包含定位字段但不泄露密码") {
  const DatabaseConfig config{"db host", 5433, "cns ' route", "route user",
                              "secret ' value", std::chrono::seconds{7}};

  const std::string description = BuildSafeConnectionDescription(config);

  CHECK(description.find("host='db host'") != std::string::npos);
  CHECK(description.find("port='5433'") != std::string::npos);
  CHECK(description.find("dbname='cns \\' route'") != std::string::npos);
  CHECK(description.find("user='route user'") != std::string::npos);
  CHECK(description.find("secret") == std::string::npos);
  CHECK(description.find("password") == std::string::npos);
  CHECK(description.find("connect_timeout") == std::string::npos);
}

TEST_CASE("迁移版本表的检查读取和写入都锁定 public schema") {
  std::ifstream input{CNS_POSTGRES_STORE_SOURCE_FILE};
  REQUIRE(input.is_open());
  const std::string source{std::istreambuf_iterator<char>{input},
                           std::istreambuf_iterator<char>{}};

  CHECK(source.find("to_regclass('public.schema_migrations')") !=
        std::string::npos);
  CHECK(source.find("FROM public.schema_migrations") != std::string::npos);
  CHECK(source.find("INSERT INTO public.schema_migrations") !=
        std::string::npos);
}

}  // namespace
