#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <pqxx/pqxx>

#include "adapters/postgres/postgres_store.hpp"

#include <cstdlib>
#include <ranges>
#include <sstream>
#include <type_traits>
#include <unistd.h>

TEST_CASE("PostgreSQL命令仓库公开完整来源命令与清理接口") {
  using Store = cns::postgres::PostgresStore;
  CHECK(std::is_member_function_pointer_v<decltype(&Store::SyncAndLoadCommandSources)>);
  CHECK(std::is_member_function_pointer_v<decltype(&Store::LoadActiveCommands)>);
  CHECK(std::is_member_function_pointer_v<decltype(&Store::RecoverActiveControlCommands)>);
  CHECK(std::is_member_function_pointer_v<decltype(&Store::FindCommand)>);
  CHECK(std::is_member_function_pointer_v<decltype(&Store::FindCommandById)>);
  CHECK(std::is_member_function_pointer_v<decltype(&Store::InsertCommand)>);
  CHECK(std::is_member_function_pointer_v<decltype(&Store::TransitionCommand)>);
  CHECK(std::is_member_function_pointer_v<decltype(&Store::DeleteExpiredTerminalCommands)>);
}

TEST_CASE("显式启用时真实数据库完成来源同步命令迁移和限量清理") {
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
  const cns::config::DatabaseConfig config{
      host, static_cast<std::uint16_t>(std::stoi(port)), name, user, password,
      std::chrono::seconds{3}};
  auto store = cns::postgres::PostgresStore::Connect(config, [](std::string) {});
  REQUIRE(store);

  const auto token = std::to_string(static_cast<unsigned long long>(::getpid())) +
                     std::to_string(static_cast<unsigned long long>(
                         std::chrono::steady_clock::now().time_since_epoch().count()));
  const std::string source = "test-" + token.substr(token.size() > 40 ? token.size() - 40 : 0);
  pqxx::connection cleanup{cns::postgres::BuildConnectionString(config)};
  struct Guard {
    pqxx::connection& connection;
    std::string source;
    ~Guard() {
      try {
        pqxx::work tx{connection};
        tx.exec("DELETE FROM commands WHERE source_id = $1", pqxx::params{source});
        tx.exec("DELETE FROM command_sources WHERE source_id = $1", pqxx::params{source});
        tx.commit();
      } catch (...) {
      }
    }
  } guard{cleanup, source};

  const cns::config::FixedSourceConfig fixed{
      source, cns::config::FixedSourceKind::kHostApp};
  auto sources = (*store)->SyncAndLoadCommandSources({fixed});
  REQUIRE(sources);
  CHECK(std::ranges::any_of(*sources, [&](const auto& item) {
    return item.source_id == source && item.enabled &&
           item.kind == cns::command::SourceKind::kHostApp;
  }));
  REQUIRE((*store)->SyncAndLoadCommandSources({fixed}));
  REQUIRE_FALSE((*store)->SyncAndLoadCommandSources(
      {{source, cns::config::FixedSourceKind::kControlCenter}}));
  REQUIRE((*store)->SyncAndLoadCommandSources({}));
  sources = (*store)->SyncAndLoadCommandSources({fixed});
  REQUIRE(sources);

  const auto at = cns::command::TimePoint{std::chrono::seconds{1'700'000'000}};
  cns::command::CommandRecord command{
      "550e8400-e29b-41d4-a716-446655440000",
      cns::command::CommandType::kConfig, source, "req-1",
      std::nullopt, {{"schema_version", 1}, {"parameters", {{"heartbeat_interval_ms", 2000}}}},
      cns::command::CommandStatus::kPending, std::nullopt, std::nullopt,
      std::nullopt, at, std::nullopt, at, std::nullopt};
  auto inserted = (*store)->InsertCommand(command);
  REQUIRE(inserted);
  auto repeated = (*store)->InsertCommand(command);
  REQUIRE(repeated);
  CHECK(repeated->command_id == command.command_id);
  auto found = (*store)->FindCommand(source, "req-1");
  REQUIRE(found);
  REQUIRE(found->has_value());
  CHECK(found->value().request_payload == command.request_payload);
  auto found_by_id = (*store)->FindCommandById(command.command_id);
  REQUIRE(found_by_id);
  REQUIRE(found_by_id->has_value());
  CHECK(found_by_id->value().command_id == command.command_id);
  CHECK(found_by_id->value().source_id == command.source_id);
  CHECK(found_by_id->value().request_id == command.request_id);
  auto missing_by_id =
      (*store)->FindCommandById("550e8400-e29b-41d4-a716-446655440099");
  REQUIRE(missing_by_id);
  CHECK_FALSE(missing_by_id->has_value());
  CHECK(inserted->command_type == cns::command::CommandType::kConfig);

  auto control = command;
  control.command_id = "550e8400-e29b-41d4-a716-446655440001";
  control.command_type = cns::command::CommandType::kControl;
  control.request_id = "req-2";
  control.request_payload = {{"schema_version", 1}, {"command", "takeoff"},
                             {"parameters", nlohmann::json::object()}};
  auto inserted_control = (*store)->InsertCommand(control);
  REQUIRE(inserted_control);
  CHECK(inserted_control->command_type == cns::command::CommandType::kControl);

  auto active = (*store)->LoadActiveCommands(2);
  REQUIRE(active);
  CHECK(active->size() == 2);

  auto recovered = (*store)->RecoverActiveControlCommands(
      at + std::chrono::seconds{1}, 1);
  REQUIRE(recovered);
  REQUIRE(recovered->size() == 1);
  CHECK(recovered->front().status ==
        cns::command::CommandStatus::kDeliveryUncertain);
  CHECK(recovered->front().error_code == "control_delivery_uncertain");

  const cns::command::CommandUpdate update{
      "target_not_found", "目标设备不存在", std::nullopt, std::nullopt,
      at, at};
  auto transitioned = (*store)->TransitionCommand(
      command.command_id, cns::command::CommandStatus::kPending,
      cns::command::CommandStatus::kFailed, update);
  REQUIRE(transitioned);
  CHECK(transitioned->status == cns::command::CommandStatus::kFailed);
  CHECK_FALSE((*store)->TransitionCommand(
      command.command_id, cns::command::CommandStatus::kPending,
      cns::command::CommandStatus::kFailed, update));
  auto deleted = (*store)->DeleteExpiredTerminalCommands(
      at + std::chrono::seconds{1}, 1);
  REQUIRE(deleted);
  CHECK(*deleted == 1);
}
