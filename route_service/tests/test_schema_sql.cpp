#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <stdexcept>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string ReadMigration(const std::string& name) {
  const auto path = std::filesystem::path{CNS_MIGRATIONS_SOURCE_DIR} / name;
  std::ifstream input{path};
  REQUIRE_MESSAGE(input.is_open(), "无法读取迁移文件：", path.string());
  return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

std::string ReadIntegrationScript(const std::string& name) {
  const auto path = std::filesystem::path{CNS_ROUTE_SERVICE_SOURCE_DIR} /
                    "tests" / "integration" / name;
  std::ifstream input{path};
  REQUIRE_MESSAGE(input.is_open(), "无法读取联调脚本：", path.string());
  return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

struct ScannedSql {
  std::string text;
  std::string code;
};

ScannedSql ScanSupportedSql(const std::string& sql) {
  ScannedSql result{sql, sql};
  bool in_string = false;
  for (std::size_t index = 0; index < sql.size();) {
    if (sql[index] == '\'' && in_string && index + 1 < sql.size() && sql[index + 1] == '\'') {
      result.code[index] = ' ';
      result.code[index + 1] = ' ';
      index += 2;
    } else if (sql[index] == '\'') {
      if (!in_string && index > 0 && std::tolower(static_cast<unsigned char>(sql[index - 1])) == 'e' &&
          (index == 1 || !(std::isalnum(static_cast<unsigned char>(sql[index - 2])) || sql[index - 2] == '_'))) {
        throw std::runtime_error{"静态扫描不支持 E 字符串"};
      }
      in_string = !in_string;
      result.code[index++] = ' ';
    } else if (!in_string && sql[index] == '-' && index + 1 < sql.size() && sql[index + 1] == '-') {
      result.code[index] = result.text[index] = ' ';
      result.code[index + 1] = result.text[index + 1] = ' ';
      index += 2;
      while (index < sql.size() && sql[index] != '\n') {
        result.code[index] = result.text[index] = ' ';
        ++index;
      }
    } else if (!in_string && index + 1 < sql.size() &&
               ((sql[index] == '/' && sql[index + 1] == '*') ||
                (sql[index] == '*' && sql[index + 1] == '/'))) {
      throw std::runtime_error{"静态扫描不支持块注释"};
    } else if (!in_string && sql[index] == '$') {
      throw std::runtime_error{"静态扫描不支持 dollar quote"};
    } else if (!in_string && sql[index] == '"') {
      throw std::runtime_error{"静态扫描不支持引用标识符"};
    } else {
      if (in_string) {
        result.code[index] = ' ';
      } else {
        result.code[index] = static_cast<char>(std::tolower(static_cast<unsigned char>(result.code[index])));
      }
      result.text[index] = static_cast<char>(std::tolower(static_cast<unsigned char>(result.text[index])));
      ++index;
    }
  }
  if (in_string) {
    throw std::runtime_error{"SQL 字符串未闭合"};
  }
  return result;
}

std::string Normalize(std::string sql) {
  std::transform(sql.begin(), sql.end(), sql.begin(), [](const unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  std::string result;
  result.reserve(sql.size());
  bool previous_space = false;
  for (const unsigned char ch : sql) {
    const bool space = std::isspace(ch) != 0;
    if (!space || !previous_space) {
      result.push_back(space ? ' ' : static_cast<char>(ch));
    }
    previous_space = space;
  }
  return result;
}

std::size_t FindClosingParenthesis(const std::string& sql, const std::size_t opening) {
  int depth = 0;
  bool in_string = false;
  for (std::size_t index = opening; index < sql.size(); ++index) {
    if (sql[index] == '\'' && in_string && index + 1 < sql.size() && sql[index + 1] == '\'') {
      ++index;
    } else if (sql[index] == '\'') {
      in_string = !in_string;
    } else if (!in_string && sql[index] == '(') {
      ++depth;
    } else if (!in_string && sql[index] == ')' && --depth == 0) {
      return index;
    }
  }
  FAIL("CREATE TABLE 定义缺少右括号");
  return std::string::npos;
}

std::map<std::string, std::string> ParseTables(const std::string& source) {
  const auto scanned = ScanSupportedSql(source);
  const std::regex any_declaration{R"(\bcreate(?:\s+[a-z_][a-z0-9_]*)*\s+table\b)"};
  const std::regex declaration{
      R"(\bcreate\s+table\s+((?:public\.)?[a-z_][a-z0-9_]*)\s*\()"};
  std::map<std::string, std::string> tables;
  for (auto match = std::sregex_iterator{scanned.code.begin(), scanned.code.end(), any_declaration};
       match != std::sregex_iterator{}; ++match) {
    std::smatch supported;
    const auto begin = scanned.code.cbegin() + match->position();
    if (!std::regex_search(begin, scanned.code.cend(), supported, declaration,
                           std::regex_constants::match_continuous)) {
      throw std::runtime_error{"CREATE TABLE 使用了静态扫描不支持的形式"};
    }
    const auto opening = static_cast<std::size_t>(match->position() + supported.length() - 1);
    const auto closing = FindClosingParenthesis(scanned.code, opening);
    const auto table = Normalize(scanned.text.substr(opening + 1, closing - opening - 1));
    if (!tables.emplace(supported[1].str(), table).second) {
      throw std::runtime_error{"迁移重复创建表"};
    }
  }
  return tables;
}

std::vector<std::string> SplitDefinitions(const std::string& table) {
  std::vector<std::string> definitions;
  std::size_t start = 0;
  int depth = 0;
  bool in_string = false;
  for (std::size_t index = 0; index <= table.size(); ++index) {
    const char ch = index == table.size() ? ',' : table[index];
    if (ch == '\'' && in_string && index + 1 < table.size() && table[index + 1] == '\'') {
      ++index;
    } else if (ch == '\'') {
      in_string = !in_string;
    } else if (!in_string && ch == '(') {
      ++depth;
    } else if (!in_string && ch == ')') {
      --depth;
    } else if (!in_string && ch == ',' && depth == 0) {
      auto definition = table.substr(start, index - start);
      const auto first = definition.find_first_not_of(' ');
      const auto last = definition.find_last_not_of(' ');
      definitions.push_back(definition.substr(first, last - first + 1));
      start = index + 1;
    }
  }
  return definitions;
}

std::string Column(const std::string& table, const std::string& name, const std::string& type) {
  const std::regex signature{"^" + name + R"(\s+)" + type + R"((?:\s|$))"};
  for (const auto& definition : SplitDefinitions(table)) {
    if (std::regex_search(definition, signature)) {
      return definition;
    }
  }
  FAIL_CHECK("表中缺少字段或字段类型错误：" << name << " " << type);
  return {};
}

void CheckParts(const std::string& definition, const std::initializer_list<std::string_view> parts) {
  for (const auto part : parts) {
    CAPTURE(part);
    CHECK(definition.find(part) != std::string::npos);
  }
}

void CheckTableNames(const std::map<std::string, std::string>& tables,
                     const std::set<std::string>& expected) {
  std::set<std::string> actual;
  for (const auto& [name, unused] : tables) {
    static_cast<void>(unused);
    actual.insert(name);
  }
  CHECK(actual == expected);
}

std::string Compact(std::string text) {
  text.erase(std::remove_if(text.begin(), text.end(), [](const unsigned char ch) {
               return std::isspace(ch) != 0;
             }),
             text.end());
  return text;
}

void CheckFieldNames(const std::string& table, const std::set<std::string>& expected) {
  std::set<std::string> actual;
  const std::regex field{R"(^([a-z_][a-z0-9_]*)\s+)"};
  for (const auto& definition : SplitDefinitions(table)) {
    std::smatch match;
    if (std::regex_search(definition, match, field) && match[1] != "check" && match[1] != "unique" &&
        match[1] != "constraint" && match[1] != "primary" && match[1] != "foreign") {
      actual.insert(match[1].str());
    }
  }
  CHECK(actual == expected);
}

void CheckNullable(const std::string& definition) {
  CHECK(definition.find("not null") == std::string::npos);
}

void CheckExactDefinition(const std::string& table, const std::string& expected) {
  const auto compact_expected = Compact(expected);
  bool found = false;
  for (const auto& definition : SplitDefinitions(table)) {
    found = found || Compact(definition) == compact_expected;
  }
  CAPTURE(expected);
  CHECK(found);
}

}  // namespace

TEST_CASE("001 只创建迁移版本表并约束全部字段") {
  const auto tables = ParseTables(ReadMigration("001_创建迁移版本表.sql"));
  CheckTableNames(tables, {"public.schema_migrations"});
  const auto& table = tables.at("public.schema_migrations");
  CheckFieldNames(table, {"version", "name", "applied_at"});

  CheckParts(Column(table, "version", "integer"), {"primary key", "check", "version > 0"});
  CheckParts(Column(table, "name", "text"), {"not null"});
  CheckParts(Column(table, "applied_at", "timestamptz"), {"not null", "default", "current_timestamp"});
}

TEST_CASE("002 只按设计创建五张核心业务表") {
  const auto tables = ParseTables(ReadMigration("002_创建核心业务表.sql"));
  CheckTableNames(tables, {"schools", "devices", "device_latest_states", "command_sources", "commands"});
}

TEST_CASE("003 只增加命令恢复与清理部分索引") {
  const auto sql = Normalize(ReadMigration("003_增加命令扫描索引.sql"));
  CHECK(sql.find("create index commands_active_updated_idx on commands "
                 "(updated_at, command_id) where status in ('pending', "
                 "'dispatched', 'in_progress', 'delivery_uncertain')") !=
        std::string::npos);
  CHECK(sql.find("create index commands_terminal_completed_idx on commands "
                 "(completed_at, command_id) where status in ('succeeded', "
                 "'failed', 'timeout') and completed_at is not null") !=
        std::string::npos);
  CHECK(ParseTables(ReadMigration("003_增加命令扫描索引.sql")).empty());
}

TEST_CASE("004 修正活动与终态索引且不修改业务数据") {
  const auto scanned = ScanSupportedSql(
      ReadMigration("004_修正命令活动与终态索引.sql"));
  const auto sql = Normalize(scanned.text);
  CHECK(sql.find("drop index if exists commands_active_updated_idx") !=
        std::string::npos);
  CHECK(sql.find("drop index if exists commands_terminal_completed_idx") !=
        std::string::npos);
  CHECK(sql.find("create index commands_active_updated_idx on commands "
                 "(updated_at, command_id) where status in ('pending', "
                 "'dispatched', 'in_progress')") != std::string::npos);
  CHECK(sql.find("create index commands_terminal_completed_idx on commands "
                 "(completed_at, command_id) where status in ('succeeded', "
                 "'failed', 'timeout', 'delivery_uncertain') and completed_at "
                 "is not null") != std::string::npos);
  CHECK(ParseTables(scanned.text).empty());
  for (const auto* forbidden : {"truncate", "drop table", "delete from"}) {
    CAPTURE(forbidden);
    CHECK(scanned.code.find(forbidden) == std::string::npos);
  }
}

TEST_CASE("里程碑四联调脚本限定依赖与清理边界") {
  const auto script = ReadIntegrationScript("里程碑四飞控命令本机联调.sh");
  CHECK(script.find("set -euo pipefail") != std::string::npos);
  for (const auto* parameter : {"--config", "--migrations", "--broker-host",
                                "--broker-port", "--binary", "--database-url",
                                "--source-id", "--vendor-id", "--control-timeout",
                                "--check-only"}) {
    CAPTURE(parameter);
    CHECK(script.find(parameter) != std::string::npos);
  }
  CHECK(script.find("request_prefix=\"m4-control-local-\"") != std::string::npos);
  CHECK(script.find("request_id=\"${request_prefix}") != std::string::npos);
  CHECK(script.find("trap cleanup EXIT") != std::string::npos);
  CHECK(script.find("request_id LIKE :'request_prefix'") != std::string::npos);
  CHECK(script.find("DELETE FROM commands WHERE source_id = :'source_id'") !=
        std::string::npos);
  CHECK(script.find("device_set_count_before_restart") != std::string::npos);
  CHECK(script.find("device_set_count_after_restart") != std::string::npos);
  for (const auto* forbidden : {"TRUNCATE", "DROP DATABASE", "systemctl stop", "pkill"}) {
    CAPTURE(forbidden);
    CHECK(script.find(forbidden) == std::string::npos);
  }
  CHECK(script.find("DELETE FROM commands;") == std::string::npos);
}

TEST_CASE("schools 字段和约束完整") {
  const auto table = ParseTables(ReadMigration("002_创建核心业务表.sql")).at("schools");
  CheckFieldNames(table, {"school_id", "school_name", "created_at"});
  CheckParts(Column(table, "school_id", "bigserial"), {"primary key"});
  CheckParts(Column(table, "school_name", "text"), {"not null", "unique"});
  CheckParts(Column(table, "created_at", "timestamptz"), {"not null", "default", "current_timestamp"});
}

TEST_CASE("devices 字段和约束完整") {
  const auto table = ParseTables(ReadMigration("002_创建核心业务表.sql")).at("devices");
  CheckFieldNames(table, {"vendor_id", "school_id", "dcdw_label", "model_version", "provisioned_at"});
  CheckParts(Column(table, "vendor_id", "varchar\\s*\\(\\s*20\\s*\\)"), {"primary key"});
  CheckParts(Column(table, "school_id", "bigint"), {"not null", "references schools", "school_id"});
  CheckNullable(Column(table, "dcdw_label", "text"));
  CheckParts(Column(table, "model_version", "text"), {"not null", "default", "'cns v1.0'"});
  CheckParts(Column(table, "provisioned_at", "timestamptz"), {"not null", "default", "current_timestamp"});
  CheckExactDefinition(table, "unique (school_id, dcdw_label)");
}

TEST_CASE("device_latest_states 字段和约束完整") {
  const auto table = ParseTables(ReadMigration("002_创建核心业务表.sql")).at("device_latest_states");
  CheckFieldNames(table, {"vendor_id", "status", "last_seen_at", "latest_telemetry", "telemetry_received_at", "updated_at"});
  CheckParts(Column(table, "vendor_id", "varchar\\s*\\(\\s*20\\s*\\)"), {"primary key", "references devices", "vendor_id"});
  CheckParts(Column(table, "status", "text"), {"not null", "check (status in ('online', 'offline'))"});
  CheckNullable(Column(table, "last_seen_at", "timestamptz"));
  CheckNullable(Column(table, "latest_telemetry", "jsonb"));
  CheckNullable(Column(table, "telemetry_received_at", "timestamptz"));
  CheckParts(Column(table, "updated_at", "timestamptz"), {"not null", "default", "current_timestamp"});
}

TEST_CASE("command_sources 字段和来源组合约束完整") {
  const auto table = ParseTables(ReadMigration("002_创建核心业务表.sql")).at("command_sources");
  CheckFieldNames(table, {"source_id", "source_kind", "device_vendor_id", "enabled", "created_at", "updated_at"});
  CheckParts(Column(table, "source_id", "varchar\\s*\\(\\s*64\\s*\\)"), {"primary key", "check", "~", "^[a-za-z0-9._-]+$"});
  CheckParts(Column(table, "source_kind", "text"), {"not null", "check (source_kind in ('device', 'host_app', 'control_center'))"});
  const auto device_vendor_id = Column(table, "device_vendor_id", "varchar\\s*\\(\\s*20\\s*\\)");
  CheckParts(device_vendor_id, {"unique", "references devices", "vendor_id"});
  CheckNullable(device_vendor_id);
  CheckParts(Column(table, "enabled", "boolean"), {"not null", "default", "true"});
  CheckParts(Column(table, "created_at", "timestamptz"), {"not null", "default", "current_timestamp"});
  CheckParts(Column(table, "updated_at", "timestamptz"), {"not null", "default", "current_timestamp"});
  CheckExactDefinition(table, "check ((source_kind = 'device' and device_vendor_id is not null and source_id = device_vendor_id) or (source_kind in ('host_app', 'control_center') and device_vendor_id is null))");
}

TEST_CASE("commands 字段、幂等和生命周期约束完整") {
  const auto table = ParseTables(ReadMigration("002_创建核心业务表.sql")).at("commands");
  CheckFieldNames(table, {"command_id", "source_id", "request_id", "command_type", "target_vendor_id", "request_payload", "status", "error_code", "error_message", "device_ack", "created_at", "dispatched_at", "updated_at", "completed_at"});
  CheckParts(Column(table, "command_id", "uuid"), {"primary key"});
  CheckParts(Column(table, "source_id", "varchar\\s*\\(\\s*64\\s*\\)"), {"not null", "references command_sources", "source_id"});
  CheckParts(Column(table, "request_id", "varchar\\s*\\(\\s*128\\s*\\)"), {"not null"});
  CheckParts(Column(table, "command_type", "text"), {"not null", "check (command_type in ('config', 'control'))"});
  const auto target = Column(table, "target_vendor_id", "varchar\\s*\\(\\s*20\\s*\\)");
  CheckParts(target, {"references devices", "vendor_id"});
  CheckNullable(target);
  CheckParts(Column(table, "request_payload", "jsonb"), {"not null"});
  CheckParts(Column(table, "status", "text"), {"not null", "check (status in ('pending', 'dispatched', 'in_progress', 'succeeded', 'failed', 'timeout', 'delivery_uncertain'))"});
  CheckNullable(Column(table, "error_code", "text"));
  CheckNullable(Column(table, "error_message", "text"));
  CheckNullable(Column(table, "device_ack", "jsonb"));
  CheckParts(Column(table, "created_at", "timestamptz"), {"not null", "default", "current_timestamp"});
  CheckNullable(Column(table, "dispatched_at", "timestamptz"));
  CheckParts(Column(table, "updated_at", "timestamptz"), {"not null", "default", "current_timestamp"});
  CheckNullable(Column(table, "completed_at", "timestamptz"));
  CheckExactDefinition(table, "unique (source_id, request_id)");
}

TEST_CASE("迁移不包含破坏性或级联删除语句") {
  const auto sql = Normalize(ScanSupportedSql(ReadMigration("001_创建迁移版本表.sql") + "\n" +
                                              ReadMigration("002_创建核心业务表.sql")).code);
  for (const auto* forbidden : {"if not exists", "drop table", "truncate", "on delete cascade"}) {
    CAPTURE(forbidden);
    CHECK(sql.find(forbidden) == std::string::npos);
  }
}

TEST_CASE("005 扩展通用设备标识、设备类型并允许 PX4 暂不绑定学校") {
  const auto sql = Normalize(
      ScanSupportedSql(ReadMigration("005_增加通用设备类型与扩展标识.sql")).text);
  CHECK(sql.find("alter table devices alter column vendor_id type varchar(64)") !=
        std::string::npos);
  CHECK(sql.find("add column device_type text not null default 'cns_box'") !=
        std::string::npos);
  CHECK(sql.find("device_type in ('cns_box', 'flight_controller')") !=
        std::string::npos);
  CHECK(sql.find("alter table devices alter column school_id drop not null") !=
        std::string::npos);
  CHECK(sql.find("^[a-za-z0-9._:-]+$") != std::string::npos);
  CHECK(sql.find("drop table") == std::string::npos);
  CHECK(sql.find("truncate") == std::string::npos);
}

TEST_CASE("字符串中的建表文本不作为 DDL") {
  CHECK(ParseTables("SELECT 'create table fake (fake_id integer)';").empty());
}

TEST_CASE("拒绝允许子集之外的建表形式") {
  CHECK_THROWS(ParseTables("CREATE TABLE private.extra (id INTEGER);"));
  CHECK_THROWS(ParseTables("CREATE TEMP TABLE extra (id INTEGER);"));
  CHECK_THROWS(ParseTables("CREATE TEMPORARY TABLE extra (id INTEGER);"));
  CHECK_THROWS(ParseTables("CREATE UNLOGGED TABLE extra (id INTEGER);"));
  CHECK_THROWS(ParseTables("CREATE TABLE IF NOT EXISTS extra (id INTEGER);"));
  CHECK_THROWS(ParseTables("CREATE TABLE \"extra\" (id INTEGER);"));
}

TEST_CASE("拒绝静态扫描器不支持的 PostgreSQL 词法") {
  CHECK_THROWS(ParseTables("/* 块注释 */ CREATE TABLE extra (id INTEGER);"));
  CHECK_THROWS(ParseTables("SELECT $$create table fake (id integer)$$;"));
  CHECK_THROWS(ParseTables("SELECT E'create table fake (id integer)';"));
}
