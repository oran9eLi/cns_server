// 本文件负责验证命令行参数的成功模式、错误输入与组合约束。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "core/cli/command_line.hpp"

#include <array>
#include <string>
#include <string_view>

using namespace std::string_view_literals;

TEST_CASE("正常模式必须显式提供配置和迁移目录") {
  const std::array args{"route_service"sv, "--config"sv, "/tmp/config.json"sv,
                        "--migrations"sv, "/tmp/migrations"sv};
  const auto result = cns::cli::ParseCommandLine(args);
  REQUIRE(result.has_value());
  CHECK(result->config_path == "/tmp/config.json");
  CHECK(result->migrations_path == "/tmp/migrations");
  CHECK_FALSE(result->migrate_only);
}

TEST_CASE("缺少配置路径时拒绝启动") {
  const std::array args{"route_service"sv, "--migrations"sv, "/tmp/migrations"sv};
  const auto result = cns::cli::ParseCommandLine(args);
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().find("--config") != std::string::npos);
}

TEST_CASE("缺少迁移目录时拒绝启动") {
  const std::array args{"route_service"sv, "--config"sv, "/tmp/config.json"sv};
  const auto result = cns::cli::ParseCommandLine(args);
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().find("--migrations") != std::string::npos);
}

TEST_CASE("迁移模式保留路径并设置标志") {
  const std::array args{"route_service"sv, "--migrate-only"sv, "--config"sv,
                        "/tmp/config.json"sv, "--migrations"sv, "/tmp/migrations"sv};
  const auto result = cns::cli::ParseCommandLine(args);
  REQUIRE(result.has_value());
  CHECK(result->migrate_only);
  CHECK(result->config_path == "/tmp/config.json");
  CHECK(result->migrations_path == "/tmp/migrations");
}

TEST_CASE("帮助参数只能单独使用") {
  const std::array only_help{"route_service"sv, "--help"sv};
  const auto help_result = cns::cli::ParseCommandLine(only_help);
  REQUIRE(help_result.has_value());
  CHECK(help_result->show_help);
  CHECK_FALSE(cns::cli::Usage().empty());

  const std::array mixed{"route_service"sv, "--help"sv, "--migrate-only"sv};
  CHECK_FALSE(cns::cli::ParseCommandLine(mixed).has_value());
}

TEST_CASE("版本参数只能单独使用") {
  const std::array only_version{"route_service"sv, "--version"sv};
  const auto version_result = cns::cli::ParseCommandLine(only_version);
  REQUIRE(version_result.has_value());
  CHECK(version_result->show_version);

  const std::array mixed{"route_service"sv, "--version"sv, "--config"sv,
                         "/tmp/config.json"sv};
  CHECK_FALSE(cns::cli::ParseCommandLine(mixed).has_value());
}

TEST_CASE("未知参数被拒绝") {
  const std::array args{"route_service"sv, "--unknown"sv};
  const auto result = cns::cli::ParseCommandLine(args);
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().find("--unknown") != std::string::npos);
}

TEST_CASE("重复参数被拒绝") {
  const std::array args{"route_service"sv, "--config"sv, "/tmp/a.json"sv,
                        "--config"sv, "/tmp/b.json"sv, "--migrations"sv, "/tmp/m"sv};
  const auto result = cns::cli::ParseCommandLine(args);
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().find("重复") != std::string::npos);
}

TEST_CASE("缺少参数值被拒绝") {
  const std::array at_end{"route_service"sv, "--config"sv};
  CHECK_FALSE(cns::cli::ParseCommandLine(at_end).has_value());

  const std::array followed_by_option{"route_service"sv, "--config"sv,
                                      "--migrations"sv, "/tmp/m"sv};
  CHECK_FALSE(cns::cli::ParseCommandLine(followed_by_option).has_value());
}

TEST_CASE("空路径被拒绝") {
  const std::array empty_config{"route_service"sv, "--config"sv, ""sv,
                                "--migrations"sv, "/tmp/m"sv};
  CHECK_FALSE(cns::cli::ParseCommandLine(empty_config).has_value());

  const std::array empty_migrations{"route_service"sv, "--config"sv, "/tmp/c"sv,
                                    "--migrations"sv, ""sv};
  CHECK_FALSE(cns::cli::ParseCommandLine(empty_migrations).has_value());
}

TEST_CASE("无参数时拒绝启动") {
  const std::array args{"route_service"sv};
  CHECK_FALSE(cns::cli::ParseCommandLine(args).has_value());
}
