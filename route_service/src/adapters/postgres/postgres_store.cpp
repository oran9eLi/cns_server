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

double ToUnixSeconds(std::chrono::system_clock::time_point time) {
  return std::chrono::duration<double>(time.time_since_epoch()).count();
}

device::TimePoint FromUnixSeconds(double seconds) {
  return device::TimePoint{std::chrono::duration_cast<device::TimePoint::duration>(
      std::chrono::duration<double>{seconds})};
}

std::string DatabaseStatus(device::Status status) {
  return status == device::Status::kOnline ? "online" : "offline";
}

device::Status DeviceStatus(std::string_view status) {
  return status == "online" ? device::Status::kOnline
                            : device::Status::kOffline;
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
             EXTRACT(EPOCH FROM st.last_seen_at)::double precision,
             st.latest_telemetry::text,
             EXTRACT(EPOCH FROM st.telemetry_received_at)::double precision
      FROM devices AS d
      JOIN schools AS s ON s.school_id = d.school_id
      JOIN device_latest_states AS st ON st.vendor_id = d.vendor_id
      ORDER BY d.vendor_id
    )sql");

    std::vector<device::DeviceRecord> records;
    records.reserve(rows.size());
    for (const pqxx::row& row : rows) {
      std::optional<nlohmann::json> telemetry;
      if (!row[7].is_null()) {
        nlohmann::json parsed = nlohmann::json::parse(row[7].as<std::string>());
        if (!parsed.is_object()) {
          return std::unexpected("读取 PostgreSQL 设备失败：遥测必须为对象");
        }
        telemetry = std::move(parsed);
      }
      records.push_back({
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
                              : std::optional{FromUnixSeconds(row[6].as<double>())},
          .latest_telemetry = std::move(telemetry),
          .telemetry_received_at =
              row[8].is_null()
                  ? std::nullopt
                  : std::optional{FromUnixSeconds(row[8].as<double>())},
          .revision = 0,
      });
    }
    return records;
  } catch (const std::exception&) {
    return std::unexpected("读取 PostgreSQL 设备失败");
  }
}

std::expected<device::DeviceRecord, std::string> PostgresStore::ProvisionDevice(
    const ProvisionRequest& request) {
  if (!request.registration.school_name || !request.registration.dcdw_label) {
    return std::unexpected("建档设备缺少学校或 DCDW 标签");
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
    const pqxx::row device_row = transaction.exec(R"sql(
      INSERT INTO devices (vendor_id, school_id, dcdw_label)
      VALUES ($1, $2, $3)
      ON CONFLICT (vendor_id) DO UPDATE SET
        school_id = EXCLUDED.school_id,
        dcdw_label = EXCLUDED.dcdw_label
      RETURNING model_version
    )sql", pqxx::params{request.registration.vendor_id, school_id,
                        *request.registration.dcdw_label}).one_row();
    const bool online = request.registration.status ==
                        protocol::RegistrationStatus::kOnline;
    const std::optional<double> last_seen =
        online ? std::optional{ToUnixSeconds(request.received_at)} : std::nullopt;
    transaction.exec(R"sql(
      INSERT INTO device_latest_states (vendor_id, status, last_seen_at)
      VALUES ($1, $2, to_timestamp($3))
      ON CONFLICT (vendor_id) DO NOTHING
    )sql", pqxx::params{request.registration.vendor_id,
                        online ? "online" : "offline", last_seen});
    transaction.exec(R"sql(
      INSERT INTO command_sources (source_id, source_kind, device_vendor_id)
      VALUES ($1, 'device', $1)
      ON CONFLICT (source_id) DO NOTHING
    )sql", pqxx::params{request.registration.vendor_id});
    transaction.commit();

    return device::DeviceRecord{
        .vendor_id = request.registration.vendor_id,
        .school_id = school_id,
        .school_name = *request.registration.school_name,
        .dcdw_label = request.registration.dcdw_label,
        .model_version = device_row[0].as<std::string>(),
        .status = online ? device::Status::kOnline : device::Status::kOffline,
        .last_seen_at = online ? std::optional{request.received_at} : std::nullopt,
        .latest_telemetry = std::nullopt,
        .telemetry_received_at = std::nullopt,
        .revision = 0,
    };
  } catch (const std::exception&) {
    return std::unexpected("PostgreSQL 设备建档失败");
  }
}

std::expected<void, std::string> PostgresStore::WriteDeviceState(
    const persistence::DesiredDeviceWrite& write) {
  if (write.write_telemetry && write.record.latest_telemetry &&
      !write.record.latest_telemetry->is_object()) {
    return std::unexpected("写入 PostgreSQL 设备状态失败：遥测必须为对象");
  }

  try {
    pqxx::work transaction{*connection_};
    if (write.write_metadata) {
      transaction.exec(R"sql(
        UPDATE devices SET dcdw_label = $2 WHERE vendor_id = $1
      )sql", pqxx::params{write.record.vendor_id, write.record.dcdw_label});
    }
    const std::optional<double> last_seen =
        write.record.last_seen_at
            ? std::optional{ToUnixSeconds(*write.record.last_seen_at)}
            : std::nullopt;
    const std::optional<std::string> telemetry =
        write.record.latest_telemetry
            ? std::optional{write.record.latest_telemetry->dump()}
            : std::nullopt;
    const std::optional<double> telemetry_received =
        write.record.telemetry_received_at
            ? std::optional{ToUnixSeconds(*write.record.telemetry_received_at)}
            : std::nullopt;
    transaction.exec(R"sql(
      UPDATE device_latest_states SET
        status = CASE WHEN $2 THEN $3 ELSE status END,
        last_seen_at = CASE WHEN $2 THEN to_timestamp($4) ELSE last_seen_at END,
        latest_telemetry = CASE WHEN $5 THEN $6::jsonb ELSE latest_telemetry END,
        telemetry_received_at =
          CASE WHEN $5 THEN to_timestamp($7) ELSE telemetry_received_at END,
        updated_at = CURRENT_TIMESTAMP
      WHERE vendor_id = $1
    )sql", pqxx::params{write.record.vendor_id, write.write_status,
                        DatabaseStatus(write.record.status), last_seen,
                        write.write_telemetry, telemetry, telemetry_received});
    transaction.commit();
    return {};
  } catch (const std::exception&) {
    return std::unexpected("写入 PostgreSQL 设备状态失败");
  }
}

}  // namespace cns::postgres
