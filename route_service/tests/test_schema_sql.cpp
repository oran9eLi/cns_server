#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

std::string ReadMigration(const std::string& name) {
  const auto path = std::filesystem::path{CNS_MIGRATIONS_SOURCE_DIR} / name;
  std::ifstream input{path};
  REQUIRE_MESSAGE(input.is_open(), "无法读取迁移文件：", path.string());
  return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
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

void CheckContains(const std::string& sql, const std::string& fragment) {
  CAPTURE(fragment);
  CHECK(sql.find(fragment) != std::string::npos);
}

}  // namespace

TEST_CASE("001 创建迁移版本表并约束全部字段") {
  const auto sql = Normalize(ReadMigration("001_创建迁移版本表.sql"));

  CheckContains(sql, "create table schema_migrations");
  CheckContains(sql, "version integer primary key");
  CheckContains(sql, "check (version > 0)");
  CheckContains(sql, "name text not null");
  CheckContains(sql, "applied_at timestamptz not null default current_timestamp");
  CHECK(sql.find("if not exists") == std::string::npos);
}

TEST_CASE("002 创建核心业务表并逐字段落实约束") {
  const auto sql = Normalize(ReadMigration("002_创建核心业务表.sql"));

  for (const auto* table : {"schools", "devices", "device_latest_states",
                            "command_sources", "commands"}) {
    CheckContains(sql, std::string{"create table "} + table);
  }

  for (const auto* fragment : {
           "school_id bigserial primary key", "school_name text not null unique",
           "created_at timestamptz not null default current_timestamp",
           "vendor_id varchar(20) primary key", "school_id bigint not null references schools(school_id)",
           "dcdw_label text", "model_version text not null default 'cns v1.0'",
           "provisioned_at timestamptz not null default current_timestamp",
           "unique (school_id, dcdw_label)",
           "vendor_id varchar(20) primary key references devices(vendor_id)",
           "status text not null check (status in ('online', 'offline'))",
           "last_seen_at timestamptz", "latest_telemetry jsonb", "telemetry_received_at timestamptz",
           "updated_at timestamptz not null default current_timestamp",
           "source_id varchar(64) primary key", "source_kind text not null",
           "device_vendor_id varchar(20) unique references devices(vendor_id)",
           "enabled boolean not null default true",
           "check (source_id ~ '^[a-za-z0-9._-]+$')",
           "source_kind = 'device' and device_vendor_id is not null and source_id = device_vendor_id",
           "source_kind in ('host_app', 'control_center') and device_vendor_id is null",
           "command_id uuid primary key", "source_id varchar(64) not null references command_sources(source_id)",
           "request_id varchar(128) not null", "command_type text not null",
           "target_vendor_id varchar(20) references devices(vendor_id)",
           "request_payload jsonb not null", "error_code text", "error_message text", "device_ack jsonb",
           "dispatched_at timestamptz", "completed_at timestamptz", "unique (source_id, request_id)"}) {
    CheckContains(sql, fragment);
  }
}

TEST_CASE("002 限制来源类型、命令类型和完整生命周期状态") {
  const auto sql = Normalize(ReadMigration("002_创建核心业务表.sql"));

  CheckContains(sql, "check (source_kind in ('device', 'host_app', 'control_center'))");
  CheckContains(sql, "check (command_type in ('config', 'control'))");
  CheckContains(sql, "check (status in ('pending', 'dispatched', 'in_progress', 'succeeded', 'failed', 'timeout', 'delivery_uncertain'))");
}

TEST_CASE("迁移不引入越界表和破坏性或级联删除语句") {
  const auto sql = Normalize(ReadMigration("001_创建迁移版本表.sql") + "\n" +
                             ReadMigration("002_创建核心业务表.sql"));

  for (const auto* forbidden : {"source_school_permissions", "drop table", "truncate", "on delete cascade"}) {
    CAPTURE(forbidden);
    CHECK(sql.find(forbidden) == std::string::npos);
  }
}
