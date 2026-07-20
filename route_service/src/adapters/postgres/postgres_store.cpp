// 本文件实现安全连接参数、迁移版本读取和单迁移事务执行。
#include "adapters/postgres/postgres_store.hpp"

#include <pqxx/pqxx>

#include <fstream>
#include <optional>
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

std::string DatabaseStatus(device::Status status) {
  return status == device::Status::kOnline ? "online" : "offline";
}

device::Status DeviceStatus(std::string_view status) {
  return status == "online" ? device::Status::kOnline
                            : device::Status::kOffline;
}

std::expected<device::DeviceRecord, std::string> DeviceFromRow(
    const pqxx::row& row) {
  std::optional<nlohmann::json> telemetry;
  if (!row[7].is_null()) {
    nlohmann::json parsed = nlohmann::json::parse(row[7].as<std::string>());
    if (!parsed.is_object()) {
      return std::unexpected("读取 PostgreSQL 设备失败：遥测必须为对象");
    }
    telemetry = std::move(parsed);
  }
  return device::DeviceRecord{
      .vendor_id = row[0].as<std::string>(),
      .school_id = row[1].as<std::int64_t>(),
      .school_name = row[2].as<std::string>(),
      .dcdw_label = row[3].is_null()
                        ? std::nullopt
                        : std::optional{row[3].as<std::string>()},
      .model_version = row[4].as<std::string>(),
      .status = DeviceStatus(row[5].as<std::string>()),
      .last_seen_at = row[6].is_null()
                          ? std::nullopt
                          : std::optional{FromUnixMicroseconds(
                                row[6].as<std::int64_t>())},
      .latest_telemetry = std::move(telemetry),
      .telemetry_received_at =
          row[8].is_null()
              ? std::nullopt
              : std::optional{
                    FromUnixMicroseconds(row[8].as<std::int64_t>())},
      .revision = 0,
  };
}

}  // namespace

DeviceWritePlan PlanDeviceWrite(bool write_metadata, bool write_status,
                                bool write_telemetry) noexcept {
  return {write_metadata, write_status || write_telemetry};
}

std::expected<void, std::string> ValidateDeviceWrite(
    const persistence::DesiredDeviceWrite& write) {
  if (write.write_telemetry && write.record.latest_telemetry &&
      !write.record.latest_telemetry->is_object()) {
    return std::unexpected("写入 PostgreSQL 设备状态失败：遥测必须为对象");
  }
  return {};
}

std::int64_t ToUnixMicroseconds(device::TimePoint time) noexcept {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             time.time_since_epoch())
      .count();
}

device::TimePoint FromUnixMicroseconds(std::int64_t microseconds) noexcept {
  return device::TimePoint{std::chrono::microseconds{microseconds}};
}

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

bool PostgresStore::IsOpen() const noexcept {
  return connection_ != nullptr && connection_->is_open();
}

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

std::expected<std::vector<device::DeviceRecord>, std::string>
PostgresStore::LoadDevices() {
  try {
    pqxx::read_transaction transaction{*connection_};
    const pqxx::result rows = transaction.exec(R"sql(
      SELECT d.vendor_id, d.school_id, s.school_name, d.dcdw_label,
             d.model_version, st.status,
             (extract(epoch FROM st.last_seen_at)::numeric * 1000000)::bigint,
             st.latest_telemetry::text,
             (extract(epoch FROM st.telemetry_received_at)::numeric * 1000000)::bigint
      FROM devices AS d
      JOIN schools AS s ON s.school_id = d.school_id
      JOIN device_latest_states AS st ON st.vendor_id = d.vendor_id
      ORDER BY d.vendor_id
    )sql");

    std::vector<device::DeviceRecord> records;
    records.reserve(rows.size());
    for (const pqxx::row& row : rows) {
      auto record = DeviceFromRow(row);
      if (!record) return std::unexpected(record.error());
      records.push_back(std::move(*record));
    }
    return records;
  } catch (const std::exception&) {
    return std::unexpected("读取 PostgreSQL 设备失败");
  }
}

std::expected<device::DeviceRecord, std::string> PostgresStore::ProvisionDevice(
    const ProvisionRequest& request) {
  if (!request.registration.school_name) {
    return std::unexpected("建档设备缺少学校");
  }

  try {
    pqxx::work transaction{*connection_};
    const pqxx::row school = transaction.exec(R"sql(
      INSERT INTO schools (school_name) VALUES ($1)
      ON CONFLICT (school_name) DO UPDATE
        SET school_name = EXCLUDED.school_name
      RETURNING school_id
    )sql", pqxx::params{*request.registration.school_name}).one_row();
    const auto school_id = school[0].as<std::int64_t>();
    transaction.exec(R"sql(
      INSERT INTO devices (vendor_id, school_id, dcdw_label)
      VALUES ($1, $2, $3)
      ON CONFLICT (vendor_id) DO NOTHING
    )sql", pqxx::params{request.registration.vendor_id, school_id,
                        request.registration.dcdw_label});
    const bool online = request.registration.status ==
                        protocol::RegistrationStatus::kOnline;
    const std::optional<std::int64_t> last_seen =
        online ? std::optional{ToUnixMicroseconds(request.received_at)}
               : std::nullopt;
    transaction.exec(R"sql(
      INSERT INTO device_latest_states (vendor_id, status, last_seen_at)
      VALUES ($1, $2,
              TIMESTAMPTZ 'epoch' + $3 * INTERVAL '1 microsecond')
      ON CONFLICT (vendor_id) DO NOTHING
    )sql", pqxx::params{request.registration.vendor_id,
                        online ? "online" : "offline", last_seen});
    transaction.exec(R"sql(
      INSERT INTO command_sources (source_id, source_kind, device_vendor_id)
      VALUES ($1, 'device', $1)
      ON CONFLICT (source_id) DO NOTHING
    )sql", pqxx::params{request.registration.vendor_id});
    const pqxx::row actual = transaction.exec(R"sql(
      SELECT d.vendor_id, d.school_id, s.school_name, d.dcdw_label,
             d.model_version, st.status,
             (extract(epoch FROM st.last_seen_at)::numeric * 1000000)::bigint,
             st.latest_telemetry::text,
             (extract(epoch FROM st.telemetry_received_at)::numeric * 1000000)::bigint
      FROM devices AS d
      JOIN schools AS s ON s.school_id = d.school_id
      JOIN device_latest_states AS st ON st.vendor_id = d.vendor_id
      WHERE d.vendor_id = $1
    )sql", pqxx::params{request.registration.vendor_id}).one_row();
    auto record = DeviceFromRow(actual);
    if (!record) return std::unexpected(record.error());
    transaction.commit();
    return record;
  } catch (const std::exception&) {
    return std::unexpected("PostgreSQL 设备建档失败");
  }
}

std::expected<void, std::string> PostgresStore::WriteDeviceState(
    const persistence::DesiredDeviceWrite& write) {
  if (auto validation = ValidateDeviceWrite(write); !validation) return validation;
  const DeviceWritePlan plan = PlanDeviceWrite(
      write.write_metadata, write.write_status, write.write_telemetry);
  if (!plan.update_metadata && !plan.update_latest_state) return {};

  try {
    pqxx::work transaction{*connection_};
    if (plan.update_metadata) {
      transaction.exec(R"sql(
        UPDATE devices SET dcdw_label = $2 WHERE vendor_id = $1
      )sql", pqxx::params{write.record.vendor_id, write.record.dcdw_label});
    }
    if (plan.update_latest_state) {
      const std::optional<std::int64_t> last_seen =
          write.record.last_seen_at
              ? std::optional{ToUnixMicroseconds(*write.record.last_seen_at)}
              : std::nullopt;
      const std::optional<std::string> telemetry =
          write.record.latest_telemetry
              ? std::optional{write.record.latest_telemetry->dump()}
              : std::nullopt;
      const std::optional<std::int64_t> telemetry_received =
          write.record.telemetry_received_at
              ? std::optional{
                    ToUnixMicroseconds(*write.record.telemetry_received_at)}
              : std::nullopt;
      transaction.exec(R"sql(
        UPDATE device_latest_states SET
          status = CASE WHEN $2 THEN $3 ELSE status END,
          last_seen_at = CASE WHEN $2 THEN
            TIMESTAMPTZ 'epoch' + $4 * INTERVAL '1 microsecond'
            ELSE last_seen_at END,
          latest_telemetry =
            CASE WHEN $5 THEN $6::jsonb ELSE latest_telemetry END,
          telemetry_received_at = CASE WHEN $5 THEN
            TIMESTAMPTZ 'epoch' + $7 * INTERVAL '1 microsecond'
            ELSE telemetry_received_at END,
          updated_at = CURRENT_TIMESTAMP
        WHERE vendor_id = $1
      )sql", pqxx::params{write.record.vendor_id, write.write_status,
                          DatabaseStatus(write.record.status), last_seen,
                          write.write_telemetry, telemetry,
                          telemetry_received});
    }
    transaction.commit();
    return {};
  } catch (const std::exception&) {
    return std::unexpected("写入 PostgreSQL 设备状态失败");
  }
}

}  // namespace cns::postgres
