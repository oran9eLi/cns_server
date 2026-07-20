// 本文件验证 PostgreSQL 设备存储的公开接口和 SQL 安全契约。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <pqxx/pqxx>

#include "adapters/postgres/postgres_store.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>

namespace {

using namespace std::chrono_literals;

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

TEST_CASE("设备状态写入计划严格遵循字段标志") {
  using cns::postgres::PlanDeviceWrite;

  CHECK(PlanDeviceWrite(false, false, false) ==
        cns::postgres::DeviceWritePlan{false, false});
  CHECK(PlanDeviceWrite(true, false, false) ==
        cns::postgres::DeviceWritePlan{true, false});
  CHECK(PlanDeviceWrite(false, true, false) ==
        cns::postgres::DeviceWritePlan{false, true});
  CHECK(PlanDeviceWrite(false, false, true) ==
        cns::postgres::DeviceWritePlan{false, true});
  CHECK(PlanDeviceWrite(true, true, true) ==
        cns::postgres::DeviceWritePlan{true, true});
}

TEST_CASE("遥测校验仅约束被请求写入的JSON且错误不回显payload") {
  cns::persistence::DesiredDeviceWrite write{};
  write.record.latest_telemetry = nlohmann::json::array({"secret-payload"});
  CHECK(cns::postgres::ValidateDeviceWrite(write).has_value());

  write.write_telemetry = true;
  const auto invalid = cns::postgres::ValidateDeviceWrite(write);
  REQUIRE_FALSE(invalid.has_value());
  CHECK(invalid.error().find("secret-payload") == std::string::npos);

  write.record.latest_telemetry = nlohmann::json{{"value", 1}};
  CHECK(cns::postgres::ValidateDeviceWrite(write).has_value());
  write.record.latest_telemetry = std::nullopt;
  CHECK(cns::postgres::ValidateDeviceWrite(write).has_value());
}

TEST_CASE("系统时间以整数微秒精确往返") {
  using cns::postgres::FromUnixMicroseconds;
  using cns::postgres::ToUnixMicroseconds;
  const std::int64_t cases[] = {0, 1, -1, 999, -999, 1000, -1000,
                                1'234'567'890'123, -1'234'567'890'123};
  for (const std::int64_t value : cases) {
    CAPTURE(value);
    CHECK(ToUnixMicroseconds(FromUnixMicroseconds(value)) == value);
  }
  CHECK(ToUnixMicroseconds(cns::device::TimePoint{1234ms + 567us}) ==
        1'234'567);
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
  CHECK(source.find("ON CONFLICT (source_id) DO NOTHING") != std::string::npos);
  CHECK(source.find("pqxx::params{request.registration.vendor_id, school_id,\n"
                    "                        request.registration.dcdw_label}") !=
        std::string::npos);
  CHECK(source.find("SELECT d.vendor_id, d.school_id, s.school_name") !=
        std::string::npos);
  const auto school = source.find("INSERT INTO schools");
  const auto device = source.find("INSERT INTO devices", school);
  const auto state = source.find("INSERT INTO device_latest_states", device);
  const auto source_insert = source.find("INSERT INTO command_sources", state);
  const auto actual = source.find("SELECT d.vendor_id", source_insert);
  const auto commit = source.find("transaction.commit()", actual);
  CHECK(school < device);
  CHECK(device < state);
  CHECK(state < source_insert);
  CHECK(source_insert < actual);
  CHECK(actual < commit);
}

TEST_CASE("状态写入参数化并由数据库转换JSONB和时间") {
  const std::string source = StoreSource();

  CHECK(source.find("$6::jsonb") != std::string::npos);
  CHECK(source.find("TIMESTAMPTZ 'epoch' + $4 * INTERVAL '1 microsecond'") !=
        std::string::npos);
  CHECK(source.find("extract(epoch FROM st.last_seen_at)::numeric * 1000000") !=
        std::string::npos);
  CHECK(source.find("duration<double>") == std::string::npos);
  CHECK(source.find("dump()") != std::string::npos);
}

TEST_CASE("最新状态更新只在状态或遥测标志置位时执行") {
  const std::string source = StoreSource();

  CHECK(source.find("if (plan.update_latest_state)") != std::string::npos);
  CHECK(source.find("if (!plan.update_metadata && !plan.update_latest_state)") !=
        std::string::npos);
}

TEST_CASE("非对象遥测在进入数据库事务前被拒绝") {
  const std::string source = StoreSource();
  const auto validation = source.find("!write.record.latest_telemetry->is_object()");
  const auto transaction = source.find("pqxx::work transaction", validation);
  REQUIRE(validation != std::string::npos);
  REQUIRE(transaction != std::string::npos);
  CHECK(validation < transaction);
}

TEST_CASE("显式启用时真实数据库支持无角色号及冲突实值") {
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

  constexpr std::string_view vendor = "codex-task5-no-role";
  pqxx::connection cleanup_connection{
      cns::postgres::BuildConnectionString(config)};
  const auto cleanup = [&] {
    pqxx::work transaction{cleanup_connection};
    transaction.exec("DELETE FROM command_sources WHERE source_id = $1",
                     pqxx::params{vendor});
    transaction.exec("DELETE FROM device_latest_states WHERE vendor_id = $1",
                     pqxx::params{vendor});
    transaction.exec("DELETE FROM devices WHERE vendor_id = $1",
                     pqxx::params{vendor});
    transaction.exec(
        "DELETE FROM schools WHERE school_name IN ($1, $2) "
        "AND NOT EXISTS (SELECT 1 FROM devices "
        "WHERE devices.school_id = schools.school_id)",
        pqxx::params{"codex-task5-school-a", "codex-task5-school-b"});
    transaction.commit();
  };
  cleanup();

  const auto first = (*store)->ProvisionDevice({
      .registration = {std::string{vendor},
                       cns::protocol::RegistrationStatus::kOnline,
                       "codex-task5-school-a", std::nullopt},
      .received_at = cns::postgres::FromUnixMicroseconds(1'234'567)});
  REQUIRE(first.has_value());
  CHECK_FALSE(first->dcdw_label.has_value());
  REQUIRE(first->last_seen_at.has_value());
  CHECK(cns::postgres::ToUnixMicroseconds(*first->last_seen_at) == 1'234'567);

  cns::persistence::DesiredDeviceWrite no_op{
      .record = *first,
      .revision = 1,
      .write_metadata = false,
      .write_status = false,
      .write_telemetry = false,
      .urgency = cns::persistence::Urgency::kImmediate};
  no_op.record.latest_telemetry = nlohmann::json::array({"ignored"});
  CHECK((*store)->WriteDeviceState(no_op).has_value());

  auto telemetry_write = no_op;
  telemetry_write.write_telemetry = true;
  telemetry_write.record.latest_telemetry = nlohmann::json{{"value", 7}};
  telemetry_write.record.telemetry_received_at =
      cns::postgres::FromUnixMicroseconds(2'345'678);
  CHECK((*store)->WriteDeviceState(telemetry_write).has_value());
  auto loaded = (*store)->LoadDevices();
  REQUIRE(loaded.has_value());
  const auto loaded_record = std::find_if(
      loaded->begin(), loaded->end(),
      [](const cns::device::DeviceRecord& record) {
        return record.vendor_id == vendor;
      });
  REQUIRE(loaded_record != loaded->end());
  REQUIRE(loaded_record->latest_telemetry.has_value());
  CHECK(loaded_record->latest_telemetry->at("value") == 7);
  REQUIRE(loaded_record->telemetry_received_at.has_value());
  CHECK(cns::postgres::ToUnixMicroseconds(
            *loaded_record->telemetry_received_at) == 2'345'678);

  telemetry_write.record.latest_telemetry =
      nlohmann::json::array({"secret-payload"});
  const auto invalid = (*store)->WriteDeviceState(telemetry_write);
  REQUIRE_FALSE(invalid.has_value());
  CHECK(invalid.error().find("secret-payload") == std::string::npos);

  {
    pqxx::work transaction{cleanup_connection};
    transaction.exec(
        "UPDATE command_sources SET enabled = false WHERE source_id = $1",
        pqxx::params{vendor});
    transaction.commit();
  }
  const auto conflict = (*store)->ProvisionDevice({
      .registration = {std::string{vendor},
                       cns::protocol::RegistrationStatus::kOffline,
                       "codex-task5-school-b", "should-not-replace"},
      .received_at = cns::postgres::FromUnixMicroseconds(9'999'999)});
  REQUIRE(conflict.has_value());
  CHECK(conflict->school_name == "codex-task5-school-a");
  CHECK_FALSE(conflict->dcdw_label.has_value());
  CHECK(conflict->status == cns::device::Status::kOnline);
  pqxx::read_transaction verify{cleanup_connection};
  CHECK_FALSE(verify.exec("SELECT enabled FROM command_sources WHERE source_id = $1",
                          pqxx::params{vendor}).one_field().as<bool>());
  verify.commit();
  cleanup();
}

}  // namespace
