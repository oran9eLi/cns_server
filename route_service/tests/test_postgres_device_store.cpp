// 本文件验证 PostgreSQL 设备存储的公开接口和 SQL 安全契约。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <pqxx/pqxx>

#include "adapters/postgres/postgres_store.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

std::string StoreSource() {
  std::ifstream input{CNS_POSTGRES_STORE_SOURCE_FILE};
  REQUIRE(input.is_open());
  return {std::istreambuf_iterator<char>{input},
          std::istreambuf_iterator<char>{}};
}

struct DatabaseRowsGuard {
  pqxx::connection& connection;
  std::string vendor;
  std::string school;
  std::string conflict_school;

  ~DatabaseRowsGuard() {
    try {
      pqxx::work transaction{connection};
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
          pqxx::params{school, conflict_school});
      transaction.commit();
    } catch (const std::exception&) {
    }
  }
};

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

TEST_CASE("加载遥测只接受JSON对象且安全隐藏原文") {
  CHECK_FALSE(cns::postgres::ParseDeviceTelemetry(std::nullopt).value()
                  .has_value());
  const auto object =
      cns::postgres::ParseDeviceTelemetry(std::string{R"({"value":1})"});
  REQUIRE(object.has_value());
  REQUIRE(object->has_value());
  CHECK(object->value().at("value") == 1);

  const auto array = cns::postgres::ParseDeviceTelemetry(
      std::string{R"(["secret-load-payload"])"});
  REQUIRE_FALSE(array.has_value());
  CHECK(array.error().find("secret-load-payload") == std::string::npos);
  const auto invalid = cns::postgres::ParseDeviceTelemetry(
      std::string{"secret-load-payload"});
  REQUIRE_FALSE(invalid.has_value());
  CHECK(invalid.error().find("secret-load-payload") == std::string::npos);
}

TEST_CASE("系统时间以整数微秒精确往返") {
  using cns::postgres::FromUnixMicroseconds;
  using cns::postgres::ToUnixMicroseconds;
  const std::int64_t cases[] = {0, 1, -1, 999, -999, 1000, -1000,
                                1'234'567'890'123, -1'234'567'890'123};
  for (const std::int64_t value : cases) {
    CAPTURE(value);
    const auto time = FromUnixMicroseconds(value);
    REQUIRE(time.has_value());
    CHECK(ToUnixMicroseconds(*time) == value);
  }
  constexpr std::int64_t exact_integer_limit = 1LL << 53;
  for (const std::int64_t value : {exact_integer_limit - 1,
                                   exact_integer_limit,
                                   exact_integer_limit + 1,
                                   -exact_integer_limit + 1,
                                   -exact_integer_limit,
                                   -exact_integer_limit - 1}) {
    CAPTURE(value);
    const auto time = FromUnixMicroseconds(value);
    REQUIRE(time.has_value());
    CHECK(ToUnixMicroseconds(*time) == value);
  }
  CHECK(ToUnixMicroseconds(cns::device::TimePoint{1234ms + 567us}) ==
        1'234'567);
  CHECK_FALSE(FromUnixMicroseconds(std::numeric_limits<std::int64_t>::max())
                  .has_value());
  CHECK_FALSE(FromUnixMicroseconds(std::numeric_limits<std::int64_t>::min())
                  .has_value());
}

TEST_CASE("加载设备使用学校设备和最新状态的全量连接") {
  const std::string source = StoreSource();

  CHECK(source.find("FROM devices") != std::string::npos);
  CHECK(source.find("JOIN schools") != std::string::npos);
  CHECK(source.find("JOIN device_latest_states") != std::string::npos);
}

TEST_CASE("建档在单个工作事务写入四张表且不重新启用冲突来源") {
  const std::string source = StoreSource();
  const auto provision_begin = source.find("PostgresStore::ProvisionDevice");
  const auto provision_end = source.find("PostgresStore::WriteDeviceState",
                                         provision_begin);
  REQUIRE(provision_begin != std::string::npos);
  REQUIRE(provision_end != std::string::npos);
  const auto provision = source.substr(provision_begin,
                                       provision_end - provision_begin);

  CHECK(source.find("INSERT INTO schools") != std::string::npos);
  CHECK(source.find("INSERT INTO devices") != std::string::npos);
  CHECK(source.find("INSERT INTO device_latest_states") != std::string::npos);
  CHECK(source.find("INSERT INTO command_sources") != std::string::npos);
  CHECK(provision.find("enabled = true") == std::string::npos);
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
  CHECK(source.find("$4::bigint / 1000000 * INTERVAL '1 second'") !=
        std::string::npos);
  CHECK(source.find("$4::bigint % 1000000 * INTERVAL '1 microsecond'") !=
        std::string::npos);
  CHECK(source.find("$7::bigint / 1000000 * INTERVAL '1 second'") !=
        std::string::npos);
  CHECK(source.find("$7::bigint % 1000000 * INTERVAL '1 microsecond'") !=
        std::string::npos);
  CHECK(source.find("$3::bigint / 1000000 * INTERVAL '1 second'") !=
        std::string::npos);
  CHECK(source.find("$3::bigint % 1000000 * INTERVAL '1 microsecond'") !=
        std::string::npos);
  CHECK(source.find("$3 * INTERVAL '1 microsecond'") == std::string::npos);
  CHECK(source.find("$4 * INTERVAL '1 microsecond'") == std::string::npos);
  CHECK(source.find("$7 * INTERVAL '1 microsecond'") == std::string::npos);
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
  CHECK(source.find("last_seen_at = CASE WHEN $2 THEN") != std::string::npos);
  CHECK(source.find("updated_at = CURRENT_TIMESTAMP") != std::string::npos);
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

  pqxx::connection cleanup_connection{
      cns::postgres::BuildConnectionString(config)};
  const auto token = static_cast<std::uint64_t>(
                         std::chrono::steady_clock::now()
                             .time_since_epoch()
                             .count()) ^
                     static_cast<std::uint64_t>(::getpid());
  std::ostringstream vendor_builder;
  vendor_builder << "t5" << std::hex << std::setw(18) << std::setfill('0')
                 << token;
  const std::string vendor = vendor_builder.str();
  REQUIRE(vendor.size() == 20);
  const std::string school = "codex-task5-school-" + vendor;
  const std::string conflict_school = school + "-conflict";
  {
    pqxx::read_transaction transaction{cleanup_connection};
    REQUIRE(transaction
                .exec("SELECT count(*) FROM devices WHERE vendor_id = $1",
                      pqxx::params{vendor})
                .one_field()
                .as<int>() == 0);
    REQUIRE(
        transaction
            .exec("SELECT count(*) FROM schools WHERE school_name IN ($1, $2)",
                  pqxx::params{school, conflict_school})
            .one_field()
            .as<int>() == 0);
  }
  DatabaseRowsGuard cleanup{cleanup_connection, vendor, school,
                            conflict_school};

  const auto first_time = cns::postgres::FromUnixMicroseconds(1'234'567);
  REQUIRE(first_time.has_value());

  const auto first = (*store)->ProvisionDevice({
      .registration = {vendor, cns::protocol::RegistrationStatus::kOnline,
                       school, std::nullopt},
      .received_at = *first_time});
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
  const auto telemetry_time = cns::postgres::FromUnixMicroseconds(2'345'678);
  REQUIRE(telemetry_time.has_value());
  telemetry_write.record.telemetry_received_at = *telemetry_time;
  CHECK((*store)->WriteDeviceState(telemetry_write).has_value());
  auto loaded = (*store)->LoadDevices();
  REQUIRE(loaded.has_value());
  const auto loaded_record = std::find_if(
      loaded->begin(), loaded->end(),
      [&vendor](const cns::device::DeviceRecord& record) {
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
  const auto conflict_time = cns::postgres::FromUnixMicroseconds(9'999'999);
  REQUIRE(conflict_time.has_value());
  const auto conflict = (*store)->ProvisionDevice({
      .registration = {vendor, cns::protocol::RegistrationStatus::kOffline,
                       conflict_school, "should-not-replace"},
      .received_at = *conflict_time});
  REQUIRE(conflict.has_value());
  CHECK(conflict->school_name == school);
  CHECK_FALSE(conflict->dcdw_label.has_value());
  CHECK(conflict->status == cns::device::Status::kOnline);
  {
    pqxx::read_transaction verify{cleanup_connection};
    CHECK_FALSE(
        verify
            .exec("SELECT enabled FROM command_sources WHERE source_id = $1",
                  pqxx::params{vendor})
            .one_field()
            .as<bool>());
  }
  {
    pqxx::work transaction{cleanup_connection};
    transaction.exec(
        "UPDATE device_latest_states SET latest_telemetry = $2::jsonb "
        "WHERE vendor_id = $1",
        pqxx::params{vendor, "[\"secret-load-payload\"]"});
    transaction.commit();
  }
  const auto invalid_load = (*store)->LoadDevices();
  REQUIRE_FALSE(invalid_load.has_value());
  CHECK(invalid_load.error().find("secret-load-payload") == std::string::npos);
}

}  // namespace
