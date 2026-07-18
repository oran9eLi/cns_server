// 本文件实现安全连接参数、迁移版本读取和单迁移事务执行。
#include "adapters/postgres/postgres_store.hpp"

#include <pqxx/pqxx>

#include <fstream>
#include <sstream>
#include <utility>

namespace cns::postgres {
namespace {

std::string QuoteConnectionValue(std::string_view value) {
  std::string quoted{"'"};
  quoted.reserve(value.size() + 2);
  for (const char character : value) {
    if (character == '\\' || character == '\'') quoted.push_back('\\');
    quoted.push_back(character);
  }
  quoted.push_back('\'');
  return quoted;
}

std::string Parameter(std::string_view name, std::string_view value) {
  return std::string{name} + '=' + QuoteConnectionValue(value);
}

std::string MigrationContext(const migration::Migration& migration) {
  return "迁移版本 " + std::to_string(migration.version) + "（" + migration.name +
         "）";
}

}  // namespace

std::string BuildSafeConnectionDescription(const config::DatabaseConfig& config) {
  return Parameter("host", config.host) + ' ' +
         Parameter("port", std::to_string(config.port)) + ' ' +
         Parameter("dbname", config.name) + ' ' + Parameter("user", config.user);
}

std::string BuildConnectionString(const config::DatabaseConfig& config) {
  return BuildSafeConnectionDescription(config) + ' ' +
         Parameter("password", config.password) + ' ' +
         Parameter("connect_timeout",
                   std::to_string(config.connect_timeout.count()));
}

PostgresStore::PostgresStore(std::unique_ptr<pqxx::connection> connection,
                             logging::Logger& logger)
    : connection_(std::move(connection)), logger_(logger) {}

PostgresStore::~PostgresStore() = default;

std::expected<std::unique_ptr<PostgresStore>, std::string> PostgresStore::Connect(
    const config::DatabaseConfig& config, logging::Logger& logger) {
  const std::string safe_description = BuildSafeConnectionDescription(config);
  try {
    auto connection =
        std::make_unique<pqxx::connection>(BuildConnectionString(config));
    logger.Info("已连接 PostgreSQL：" + safe_description);
    return std::unique_ptr<PostgresStore>(
        new PostgresStore(std::move(connection), logger));
  } catch (const std::exception&) {
    return std::unexpected("连接 PostgreSQL 失败：" + safe_description);
  }
}

std::expected<std::vector<migration::AppliedMigration>, std::string>
PostgresStore::ReadAppliedMigrations() {
  try {
    pqxx::read_transaction transaction{*connection_};
    const pqxx::row table =
        transaction.exec("SELECT to_regclass('public.schema_migrations')")
            .one_row();
    if (table[0].is_null()) return std::vector<migration::AppliedMigration>{};

    std::vector<migration::AppliedMigration> migrations;
    const pqxx::result rows = transaction.exec(
        "SELECT version, name FROM public.schema_migrations ORDER BY version");
    migrations.reserve(rows.size());
    for (const pqxx::row& row : rows) {
      migrations.push_back(
          {row[0].as<int>(), row[1].as<std::string>()});
    }
    return migrations;
  } catch (const std::exception&) {
    return std::unexpected("读取 PostgreSQL 迁移版本失败");
  }
}

std::expected<void, std::string> PostgresStore::ApplyMigration(
    const migration::Migration& migration) {
  std::ifstream input{migration.file, std::ios::binary};
  if (!input.is_open()) {
    return std::unexpected("读取" + MigrationContext(migration) + "文件失败");
  }
  std::ostringstream content;
  content << input.rdbuf();
  if (input.bad()) {
    return std::unexpected("读取" + MigrationContext(migration) + "文件失败");
  }

  try {
    pqxx::work transaction{*connection_};
    transaction.exec(content.str());
    transaction.exec(
        "INSERT INTO public.schema_migrations (version, name) VALUES ($1, $2)",
        pqxx::params{migration.version, migration.name});
    transaction.commit();
    logger_.Info("已执行" + MigrationContext(migration));
    return {};
  } catch (const std::exception&) {
    return std::unexpected("执行" + MigrationContext(migration) + "失败");
  }
}

}  // namespace cns::postgres
