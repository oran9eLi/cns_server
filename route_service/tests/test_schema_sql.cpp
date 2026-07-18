#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
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

std::string StripLineComments(const std::string& sql) {
  std::string result;
  result.reserve(sql.size());
  bool in_string = false;
  for (std::size_t index = 0; index < sql.size();) {
    if (sql[index] == '\'' && in_string && index + 1 < sql.size() && sql[index + 1] == '\'') {
      result.append("''");
      index += 2;
    } else if (sql[index] == '\'') {
      in_string = !in_string;
      result.push_back(sql[index++]);
    } else if (!in_string && sql[index] == '-' && index + 1 < sql.size() && sql[index + 1] == '-') {
      index += 2;
      while (index < sql.size() && sql[index] != '\n') {
        ++index;
      }
    } else {
      result.push_back(sql[index++]);
    }
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
  const auto sql = Normalize(StripLineComments(source));
  const std::regex declaration{R"(create\s+table\s+([a-z_][a-z0-9_]*)\s*\()"};
  std::map<std::string, std::string> tables;
  for (auto match = std::sregex_iterator{sql.begin(), sql.end(), declaration};
       match != std::sregex_iterator{}; ++match) {
    const auto opening = static_cast<std::size_t>(match->position() + match->length() - 1);
    const auto closing = FindClosingParenthesis(sql, opening);
    REQUIRE_MESSAGE(tables.emplace((*match)[1].str(), sql.substr(opening + 1, closing - opening - 1)).second,
                    "迁移重复创建表：", (*match)[1].str());
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

}  // namespace

TEST_CASE("001 只创建迁移版本表并约束全部字段") {
  const auto tables = ParseTables(ReadMigration("001_创建迁移版本表.sql"));
  CheckTableNames(tables, {"schema_migrations"});
  const auto& table = tables.at("schema_migrations");

  CheckParts(Column(table, "version", "integer"), {"primary key", "check", "version > 0"});
  CheckParts(Column(table, "name", "text"), {"not null"});
  CheckParts(Column(table, "applied_at", "timestamptz"), {"not null", "default", "current_timestamp"});
}

TEST_CASE("002 只按设计创建五张核心业务表") {
  const auto tables = ParseTables(ReadMigration("002_创建核心业务表.sql"));
  CheckTableNames(tables, {"schools", "devices", "device_latest_states", "command_sources", "commands"});
}

TEST_CASE("schools 字段和约束完整") {
  const auto table = ParseTables(ReadMigration("002_创建核心业务表.sql")).at("schools");
  CheckParts(Column(table, "school_id", "bigserial"), {"primary key"});
  CheckParts(Column(table, "school_name", "text"), {"not null", "unique"});
  CheckParts(Column(table, "created_at", "timestamptz"), {"not null", "default", "current_timestamp"});
}

TEST_CASE("devices 字段和约束完整") {
  const auto table = ParseTables(ReadMigration("002_创建核心业务表.sql")).at("devices");
  CheckParts(Column(table, "vendor_id", "varchar\\s*\\(\\s*20\\s*\\)"), {"primary key"});
  CheckParts(Column(table, "school_id", "bigint"), {"not null", "references schools", "school_id"});
  CHECK(Column(table, "dcdw_label", "text").find("not null") == std::string::npos);
  CheckParts(Column(table, "model_version", "text"), {"not null", "default", "'cns v1.0'"});
  CheckParts(Column(table, "provisioned_at", "timestamptz"), {"not null", "default", "current_timestamp"});
  CheckParts(table, {"unique", "school_id", "dcdw_label"});
}

TEST_CASE("device_latest_states 字段和约束完整") {
  const auto table = ParseTables(ReadMigration("002_创建核心业务表.sql")).at("device_latest_states");
  CheckParts(Column(table, "vendor_id", "varchar\\s*\\(\\s*20\\s*\\)"), {"primary key", "references devices", "vendor_id"});
  CheckParts(Column(table, "status", "text"), {"not null", "check", "'online'", "'offline'"});
  Column(table, "last_seen_at", "timestamptz");
  Column(table, "latest_telemetry", "jsonb");
  Column(table, "telemetry_received_at", "timestamptz");
  CheckParts(Column(table, "updated_at", "timestamptz"), {"not null", "default", "current_timestamp"});
}

TEST_CASE("command_sources 字段和来源组合约束完整") {
  const auto table = ParseTables(ReadMigration("002_创建核心业务表.sql")).at("command_sources");
  CheckParts(Column(table, "source_id", "varchar\\s*\\(\\s*64\\s*\\)"), {"primary key", "check", "~", "^[a-za-z0-9._-]+$"});
  CheckParts(Column(table, "source_kind", "text"), {"not null", "check", "'device'", "'host_app'", "'control_center'"});
  CheckParts(Column(table, "device_vendor_id", "varchar\\s*\\(\\s*20\\s*\\)"), {"unique", "references devices", "vendor_id"});
  CheckParts(Column(table, "enabled", "boolean"), {"not null", "default", "true"});
  CheckParts(Column(table, "created_at", "timestamptz"), {"not null", "default", "current_timestamp"});
  CheckParts(Column(table, "updated_at", "timestamptz"), {"not null", "default", "current_timestamp"});
  CheckParts(table, {"source_kind = 'device'", "device_vendor_id is not null", "source_id = device_vendor_id"});
  CheckParts(table, {"source_kind in ('host_app', 'control_center')", "device_vendor_id is null"});
}

TEST_CASE("commands 字段、幂等和生命周期约束完整") {
  const auto table = ParseTables(ReadMigration("002_创建核心业务表.sql")).at("commands");
  CheckParts(Column(table, "command_id", "uuid"), {"primary key"});
  CheckParts(Column(table, "source_id", "varchar\\s*\\(\\s*64\\s*\\)"), {"not null", "references command_sources", "source_id"});
  CheckParts(Column(table, "request_id", "varchar\\s*\\(\\s*128\\s*\\)"), {"not null"});
  CheckParts(Column(table, "command_type", "text"), {"not null", "check", "'config'", "'control'"});
  const auto target = Column(table, "target_vendor_id", "varchar\\s*\\(\\s*20\\s*\\)");
  CheckParts(target, {"references devices", "vendor_id"});
  CHECK(target.find("not null") == std::string::npos);
  CheckParts(Column(table, "request_payload", "jsonb"), {"not null"});
  CheckParts(Column(table, "status", "text"), {"not null", "check", "'pending'", "'dispatched'", "'in_progress'", "'succeeded'", "'failed'", "'timeout'", "'delivery_uncertain'"});
  Column(table, "error_code", "text");
  Column(table, "error_message", "text");
  Column(table, "device_ack", "jsonb");
  CheckParts(Column(table, "created_at", "timestamptz"), {"not null", "default", "current_timestamp"});
  Column(table, "dispatched_at", "timestamptz");
  CheckParts(Column(table, "updated_at", "timestamptz"), {"not null", "default", "current_timestamp"});
  Column(table, "completed_at", "timestamptz");
  CheckParts(table, {"unique", "source_id", "request_id"});
}

TEST_CASE("迁移不包含破坏性或级联删除语句") {
  const auto sql = Normalize(StripLineComments(ReadMigration("001_创建迁移版本表.sql") + "\n" +
                                                ReadMigration("002_创建核心业务表.sql")));
  for (const auto* forbidden : {"if not exists", "drop table", "truncate", "on delete cascade"}) {
    CAPTURE(forbidden);
    CHECK(sql.find(forbidden) == std::string::npos);
  }
}
