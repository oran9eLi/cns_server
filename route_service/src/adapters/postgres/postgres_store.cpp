// 本文件实现安全连接参数、迁移版本读取和单迁移事务执行。
#include "adapters/postgres/postgres_store.hpp"

#include <pqxx/pqxx>

#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace cns::postgres {

OperationFailureKind ClassifyOperationFailure(bool connection_open,
                                              bool broken_connection) noexcept {
  return broken_connection || !connection_open
             ? OperationFailureKind::kUnavailable
             : OperationFailureKind::kPermanent;
}

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
  auto telemetry = ParseDeviceTelemetry(
      row[7].is_null()
          ? std::nullopt
          : std::optional<std::string>{row[7].as<std::string>()});
  if (!telemetry) return std::unexpected(telemetry.error());
  std::optional<device::TimePoint> last_seen;
  if (!row[6].is_null()) {
    auto parsed = FromUnixMicroseconds(row[6].as<std::int64_t>());
    if (!parsed) return std::unexpected(parsed.error());
    last_seen = *parsed;
  }
  std::optional<device::TimePoint> telemetry_received;
  if (!row[8].is_null()) {
    auto parsed = FromUnixMicroseconds(row[8].as<std::int64_t>());
    if (!parsed) return std::unexpected(parsed.error());
    telemetry_received = *parsed;
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
      .last_seen_at = last_seen,
      .latest_telemetry = std::move(*telemetry),
      .telemetry_received_at = telemetry_received,
      .revision = 0,
  };
}

const char* CommandStatusText(command::CommandStatus status) {
  switch (status) {
    case command::CommandStatus::kPending: return "pending";
    case command::CommandStatus::kDispatched: return "dispatched";
    case command::CommandStatus::kInProgress: return "in_progress";
    case command::CommandStatus::kSucceeded: return "succeeded";
    case command::CommandStatus::kFailed: return "failed";
    case command::CommandStatus::kTimeout: return "timeout";
    case command::CommandStatus::kDeliveryUncertain:
      return "delivery_uncertain";
  }
  return "";
}

std::expected<command::CommandStatus, std::string> ParseCommandStatus(
    std::string_view status) {
  if (status == "pending") return command::CommandStatus::kPending;
  if (status == "dispatched") return command::CommandStatus::kDispatched;
  if (status == "succeeded") return command::CommandStatus::kSucceeded;
  if (status == "failed") return command::CommandStatus::kFailed;
  if (status == "timeout") return command::CommandStatus::kTimeout;
  return std::unexpected("读取 PostgreSQL 命令失败：状态不受支持");
}

std::expected<nlohmann::json, std::string> ParseCommandJson(
    const pqxx::field& field) {
  try {
    return nlohmann::json::parse(field.as<std::string>());
  } catch (const nlohmann::json::exception&) {
    return std::unexpected("读取 PostgreSQL 命令失败：JSON 无效");
  }
}

std::expected<command::CommandRecord, std::string> CommandFromRow(
    const pqxx::row& row) {
  auto type = command::ParseCommandType(row[1].as<std::string>());
  if (!type) {
    return std::unexpected("读取 PostgreSQL 命令失败：命令类型不受支持");
  }
  auto status = ParseCommandStatus(row[6].as<std::string>());
  if (!status) return std::unexpected(status.error());
  auto request = ParseCommandJson(row[5]);
  if (!request) return std::unexpected(request.error());
  std::optional<nlohmann::json> device_ack;
  if (!row[9].is_null()) {
    auto parsed = ParseCommandJson(row[9]);
    if (!parsed) return std::unexpected(parsed.error());
    device_ack = std::move(*parsed);
  }
  auto time = [&row](std::size_t index)
      -> std::expected<command::TimePoint, std::string> {
    auto parsed = FromUnixMicroseconds(row[index].as<std::int64_t>());
    if (!parsed) return std::unexpected(parsed.error());
    return *parsed;
  };
  auto created = time(10);
  auto updated = time(12);
  if (!created || !updated) {
    return std::unexpected("读取 PostgreSQL 命令失败：时间无效");
  }
  std::optional<command::TimePoint> dispatched;
  std::optional<command::TimePoint> completed;
  if (!row[11].is_null()) {
    auto parsed = time(11);
    if (!parsed) return std::unexpected(parsed.error());
    dispatched = *parsed;
  }
  if (!row[13].is_null()) {
    auto parsed = time(13);
    if (!parsed) return std::unexpected(parsed.error());
    completed = *parsed;
  }
  return command::CommandRecord{
      row[0].as<std::string>(), *type, row[2].as<std::string>(),
      row[3].as<std::string>(),
      row[4].is_null() ? std::nullopt
                       : std::optional{row[4].as<std::string>()},
      std::move(*request), *status,
      row[7].is_null() ? std::nullopt : std::optional{row[7].as<std::string>()},
      row[8].is_null() ? std::nullopt : std::optional{row[8].as<std::string>()},
      std::move(device_ack), *created, dispatched, *updated, completed};
}

constexpr std::string_view kCommandColumns = R"sql(
  command_id::text, command_type, source_id, request_id, target_vendor_id,
  request_payload::text, status, error_code, error_message, device_ack::text,
  (extract(epoch FROM created_at)::numeric * 1000000)::bigint,
  (extract(epoch FROM dispatched_at)::numeric * 1000000)::bigint,
  (extract(epoch FROM updated_at)::numeric * 1000000)::bigint,
  (extract(epoch FROM completed_at)::numeric * 1000000)::bigint
)sql";

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

std::expected<std::optional<nlohmann::json>, std::string> ParseDeviceTelemetry(
    const std::optional<std::string>& text) {
  if (!text) return std::optional<nlohmann::json>{};
  try {
    nlohmann::json parsed = nlohmann::json::parse(*text);
    if (!parsed.is_object()) {
      return std::unexpected("读取 PostgreSQL 设备失败：遥测必须为对象");
    }
    return std::optional<nlohmann::json>{std::move(parsed)};
  } catch (const nlohmann::json::exception&) {
    return std::unexpected("读取 PostgreSQL 设备失败：遥测 JSON 无效");
  }
}

std::int64_t ToUnixMicroseconds(device::TimePoint time) noexcept {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             time.time_since_epoch())
      .count();
}

std::expected<device::TimePoint, std::string> FromUnixMicroseconds(
    std::int64_t microseconds) noexcept {
  constexpr auto minimum = std::chrono::duration_cast<std::chrono::microseconds>(
                               device::TimePoint::duration::min())
                               .count();
  constexpr auto maximum = std::chrono::duration_cast<std::chrono::microseconds>(
                               device::TimePoint::duration::max())
                               .count();
  if (microseconds < minimum || microseconds > maximum) {
    return std::unexpected("PostgreSQL 设备时间超出系统时钟范围");
  }
  return device::TimePoint{
      std::chrono::duration_cast<device::TimePoint::duration>(
          std::chrono::microseconds{microseconds})};
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
                             InfoSink info_sink)
    : connection_(std::move(connection)), info_sink_(std::move(info_sink)) {}

PostgresStore::~PostgresStore() = default;

bool PostgresStore::IsOpen() const noexcept {
  return connection_ != nullptr && connection_->is_open();
}

OperationFailureKind PostgresStore::LastOperationFailureKind() const noexcept {
  return last_failure_kind_;
}

std::expected<std::unique_ptr<PostgresStore>, std::string> PostgresStore::Connect(
    const config::DatabaseConfig& config, logging::Logger& logger) {
  return Connect(config, [&logger](std::string message) {
    logger.Info(message);
  });
}

std::expected<std::unique_ptr<PostgresStore>, std::string> PostgresStore::Connect(
    const config::DatabaseConfig& config, InfoSink info_sink) {
  const std::string safe_description = BuildSafeConnectionDescription(config);
  try {
    auto connection =
        std::make_unique<pqxx::connection>(BuildConnectionString(config));
    if (info_sink) info_sink("已连接 PostgreSQL：" + safe_description);
    return std::unique_ptr<PostgresStore>(
        new PostgresStore(std::move(connection), std::move(info_sink)));
  } catch (const std::exception&) {
    return std::unexpected("连接 PostgreSQL 失败：" + safe_description);
  }
}

void PostgresStore::SetInfoSink(InfoSink info_sink) {
  info_sink_ = std::move(info_sink);
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
  } catch (const pqxx::broken_connection&) {
    last_failure_kind_ = ClassifyOperationFailure(IsOpen(), true);
    return std::unexpected("读取 PostgreSQL 迁移版本失败");
  } catch (const std::exception&) {
    last_failure_kind_ = ClassifyOperationFailure(IsOpen(), false);
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
    if (info_sink_) info_sink_("已执行" + MigrationContext(migration));
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
    last_failure_kind_ = OperationFailureKind::kPermanent;
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
              TIMESTAMPTZ 'epoch'
                + $3::bigint / 1000000 * INTERVAL '1 second'
                + $3::bigint % 1000000 * INTERVAL '1 microsecond')
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
  } catch (const pqxx::broken_connection&) {
    last_failure_kind_ = ClassifyOperationFailure(IsOpen(), true);
    return std::unexpected("PostgreSQL 设备建档失败");
  } catch (const std::exception&) {
    last_failure_kind_ = ClassifyOperationFailure(IsOpen(), false);
    return std::unexpected("PostgreSQL 设备建档失败");
  }
}

std::expected<void, std::string> PostgresStore::WriteDeviceState(
    const persistence::DesiredDeviceWrite& write) {
  if (auto validation = ValidateDeviceWrite(write); !validation) {
    last_failure_kind_ = OperationFailureKind::kPermanent;
    return validation;
  }
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
            TIMESTAMPTZ 'epoch'
              + $4::bigint / 1000000 * INTERVAL '1 second'
              + $4::bigint % 1000000 * INTERVAL '1 microsecond'
            ELSE last_seen_at END,
          latest_telemetry =
            CASE WHEN $5 THEN $6::jsonb ELSE latest_telemetry END,
          telemetry_received_at = CASE WHEN $5 THEN
            TIMESTAMPTZ 'epoch'
              + $7::bigint / 1000000 * INTERVAL '1 second'
              + $7::bigint % 1000000 * INTERVAL '1 microsecond'
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
  } catch (const pqxx::broken_connection&) {
    last_failure_kind_ = ClassifyOperationFailure(IsOpen(), true);
    return std::unexpected("写入 PostgreSQL 设备状态失败");
  } catch (const std::exception&) {
    last_failure_kind_ = ClassifyOperationFailure(IsOpen(), false);
    return std::unexpected("写入 PostgreSQL 设备状态失败");
  }
}

std::expected<std::vector<command::CommandSource>, std::string>
PostgresStore::SyncAndLoadCommandSources(
    const std::vector<config::FixedSourceConfig>& configured_sources) {
  try {
    pqxx::work transaction{*connection_};
    const auto existing = transaction.exec(
        "SELECT source_id, source_kind, device_vendor_id, enabled "
        "FROM command_sources");
    std::unordered_map<std::string, std::string> kinds;
    for (const auto& row : existing) {
      kinds.emplace(row[0].as<std::string>(), row[1].as<std::string>());
    }
    std::unordered_set<std::string> configured_ids;
    for (const auto& source : configured_sources) {
      const std::string kind =
          source.source_kind == config::FixedSourceKind::kHostApp
              ? "host_app"
              : "control_center";
      const auto found = kinds.find(source.source_id);
      if (found != kinds.end() && found->second != kind) {
        last_failure_kind_ = OperationFailureKind::kPermanent;
        return std::unexpected("同步 PostgreSQL 命令来源失败：来源类型冲突");
      }
      configured_ids.insert(source.source_id);
    }
    for (const auto& row : existing) {
      const auto source_id = row[0].as<std::string>();
      if (row[1].as<std::string>() != "device" &&
          !configured_ids.contains(source_id)) {
        transaction.exec(
            "UPDATE command_sources SET enabled = false, "
            "updated_at = CURRENT_TIMESTAMP WHERE source_id = $1",
            pqxx::params{source_id});
      }
    }
    for (const auto& source : configured_sources) {
      const char* kind =
          source.source_kind == config::FixedSourceKind::kHostApp
              ? "host_app"
              : "control_center";
      transaction.exec(R"sql(
        INSERT INTO command_sources (source_id, source_kind, enabled)
        VALUES ($1, $2, true)
        ON CONFLICT (source_id) DO UPDATE SET
          enabled = true, updated_at = CURRENT_TIMESTAMP
      )sql", pqxx::params{source.source_id, kind});
    }
    const auto rows = transaction.exec(
        "SELECT source_id, source_kind, device_vendor_id, enabled "
        "FROM command_sources ORDER BY source_id");
    std::vector<command::CommandSource> sources;
    sources.reserve(rows.size());
    for (const auto& row : rows) {
      const auto kind = row[1].as<std::string>();
      command::SourceKind parsed_kind;
      if (kind == "device") parsed_kind = command::SourceKind::kDevice;
      else if (kind == "host_app") parsed_kind = command::SourceKind::kHostApp;
      else if (kind == "control_center") {
        parsed_kind = command::SourceKind::kControlCenter;
      } else {
        last_failure_kind_ = OperationFailureKind::kPermanent;
        return std::unexpected("读取 PostgreSQL 命令来源失败：来源类型无效");
      }
      sources.push_back({
          row[0].as<std::string>(), parsed_kind,
          row[2].is_null() ? std::nullopt
                           : std::optional{row[2].as<std::string>()},
          row[3].as<bool>()});
    }
    transaction.commit();
    return sources;
  } catch (const pqxx::broken_connection&) {
    last_failure_kind_ = ClassifyOperationFailure(IsOpen(), true);
    return std::unexpected("同步 PostgreSQL 命令来源失败");
  } catch (const std::exception&) {
    last_failure_kind_ = ClassifyOperationFailure(IsOpen(), false);
    return std::unexpected("同步 PostgreSQL 命令来源失败");
  }
}

std::expected<std::vector<command::CommandRecord>, std::string>
PostgresStore::LoadActiveCommands(std::size_t limit) {
  if (limit > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
    return std::unexpected("读取 PostgreSQL 活动命令失败：数量超出范围");
  }
  try {
    pqxx::read_transaction transaction{*connection_};
    const auto rows = transaction.exec(
        "SELECT " + std::string{kCommandColumns} +
            " FROM commands WHERE status IN ('pending', 'dispatched', "
            "'in_progress') "
            "ORDER BY updated_at, command_id LIMIT $1",
        pqxx::params{static_cast<std::int64_t>(limit)});
    std::vector<command::CommandRecord> commands;
    commands.reserve(rows.size());
    for (const auto& row : rows) {
      auto command = CommandFromRow(row);
      if (!command) return std::unexpected(command.error());
      commands.push_back(std::move(*command));
    }
    return commands;
  } catch (const pqxx::broken_connection&) {
    last_failure_kind_ = ClassifyOperationFailure(IsOpen(), true);
    return std::unexpected("读取 PostgreSQL 活动命令失败");
  } catch (const std::exception&) {
    last_failure_kind_ = ClassifyOperationFailure(IsOpen(), false);
    return std::unexpected("读取 PostgreSQL 活动命令失败");
  }
}

std::expected<std::vector<command::CommandRecord>, std::string>
PostgresStore::LoadActiveConfigCommands(std::size_t limit) {
  if (limit > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
    return std::unexpected("读取 PostgreSQL 活动命令失败：数量超出范围");
  }
  try {
    pqxx::read_transaction transaction{*connection_};
    const auto rows = transaction.exec(
        "SELECT " + std::string{kCommandColumns} +
            " FROM commands WHERE command_type = 'config' "
            "AND status IN ('pending', 'dispatched') "
            "ORDER BY updated_at, command_id LIMIT $1",
        pqxx::params{static_cast<std::int64_t>(limit)});
    std::vector<command::CommandRecord> commands;
    commands.reserve(rows.size());
    for (const auto& row : rows) {
      auto command = CommandFromRow(row);
      if (!command) return std::unexpected(command.error());
      commands.push_back(std::move(*command));
    }
    return commands;
  } catch (const pqxx::broken_connection&) {
    last_failure_kind_ = ClassifyOperationFailure(IsOpen(), true);
    return std::unexpected("读取 PostgreSQL 活动命令失败");
  } catch (const std::exception&) {
    last_failure_kind_ = ClassifyOperationFailure(IsOpen(), false);
    return std::unexpected("读取 PostgreSQL 活动命令失败");
  }
}

std::expected<std::vector<command::CommandRecord>, std::string>
PostgresStore::RecoverActiveControlCommands(command::TimePoint recovered_at,
                                             std::size_t limit) {
  if (limit > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
    return std::unexpected("恢复 PostgreSQL 飞控命令失败：数量超出范围");
  }
  try {
    pqxx::work transaction{*connection_};
    const auto rows = transaction.exec(
        "WITH active AS ("
        "SELECT command_id FROM commands WHERE command_type = 'control' "
        "AND status IN ('pending', 'dispatched', 'in_progress') "
        "ORDER BY updated_at, command_id LIMIT $2 FOR UPDATE"
        ") UPDATE commands SET status = 'delivery_uncertain', "
        "error_code = 'control_delivery_uncertain', "
        "error_message = '服务恢复后无法确认飞控命令是否已经执行', "
        "updated_at = TIMESTAMPTZ 'epoch' + $1::bigint * INTERVAL '1 microsecond', "
        "completed_at = TIMESTAMPTZ 'epoch' + $1::bigint * INTERVAL '1 microsecond' "
        "WHERE command_id IN (SELECT command_id FROM active) RETURNING " +
            std::string{kCommandColumns},
        pqxx::params{ToUnixMicroseconds(recovered_at),
                     static_cast<std::int64_t>(limit)});
    std::vector<command::CommandRecord> commands;
    commands.reserve(rows.size());
    for (const auto& row : rows) {
      auto command = CommandFromRow(row);
      if (!command) return std::unexpected(command.error());
      commands.push_back(std::move(*command));
    }
    transaction.commit();
    return commands;
  } catch (const pqxx::broken_connection&) {
    last_failure_kind_ = ClassifyOperationFailure(IsOpen(), true);
    return std::unexpected("恢复 PostgreSQL 飞控命令失败");
  } catch (const std::exception&) {
    last_failure_kind_ = ClassifyOperationFailure(IsOpen(), false);
    return std::unexpected("恢复 PostgreSQL 飞控命令失败");
  }
}

std::expected<std::optional<command::CommandRecord>, std::string>
PostgresStore::FindCommand(std::string_view source_id,
                           std::string_view request_id) {
  try {
    pqxx::read_transaction transaction{*connection_};
    const auto rows = transaction.exec(
        "SELECT " + std::string{kCommandColumns} +
            " FROM commands WHERE source_id = $1 AND request_id = $2",
        pqxx::params{source_id, request_id});
    if (rows.empty()) return std::optional<command::CommandRecord>{};
    auto command = CommandFromRow(rows.front());
    if (!command) return std::unexpected(command.error());
    return std::optional<command::CommandRecord>{std::move(*command)};
  } catch (const pqxx::broken_connection&) {
    last_failure_kind_ = ClassifyOperationFailure(IsOpen(), true);
    return std::unexpected("读取 PostgreSQL 命令失败");
  } catch (const std::exception&) {
    last_failure_kind_ = ClassifyOperationFailure(IsOpen(), false);
    return std::unexpected("读取 PostgreSQL 命令失败");
  }
}

std::expected<command::CommandRecord, std::string>
PostgresStore::InsertCommand(const command::CommandRecord& command) {
  try {
    pqxx::work transaction{*connection_};
    const auto row = transaction.exec(
        "INSERT INTO commands (command_id, source_id, request_id, "
        "command_type, target_vendor_id, request_payload, status, error_code, "
        "error_message, device_ack, created_at, dispatched_at, updated_at, "
        "completed_at) VALUES ($1::uuid, $2, $3, $4, $5, $6::jsonb, "
        "$7, $8, $9, $10::jsonb, "
        "TIMESTAMPTZ 'epoch' + $11::bigint * INTERVAL '1 microsecond', "
        "TIMESTAMPTZ 'epoch' + $12::bigint * INTERVAL '1 microsecond', "
        "TIMESTAMPTZ 'epoch' + $13::bigint * INTERVAL '1 microsecond', "
        "TIMESTAMPTZ 'epoch' + $14::bigint * INTERVAL '1 microsecond') "
        "ON CONFLICT (source_id, request_id) DO UPDATE "
        "SET request_id = EXCLUDED.request_id RETURNING " +
            std::string{kCommandColumns},
        pqxx::params{
            command.command_id, std::string{ToString(command.command_type)},
            command.source_id, command.request_id,
            command.target_vendor_id, command.request_payload.dump(),
            CommandStatusText(command.status), command.error_code,
            command.error_message,
            command.device_ack
                ? std::optional<std::string>{command.device_ack->dump()}
                : std::nullopt,
            ToUnixMicroseconds(command.created_at),
            command.dispatched_at
                ? std::optional{ToUnixMicroseconds(*command.dispatched_at)}
                : std::nullopt,
            ToUnixMicroseconds(command.updated_at),
            command.completed_at
                ? std::optional{ToUnixMicroseconds(*command.completed_at)}
                : std::nullopt})
                         .one_row();
    auto inserted = CommandFromRow(row);
    if (!inserted) return std::unexpected(inserted.error());
    transaction.commit();
    return inserted;
  } catch (const pqxx::broken_connection&) {
    last_failure_kind_ = ClassifyOperationFailure(IsOpen(), true);
    return std::unexpected("写入 PostgreSQL 命令失败");
  } catch (const std::exception&) {
    last_failure_kind_ = ClassifyOperationFailure(IsOpen(), false);
    return std::unexpected("写入 PostgreSQL 命令失败");
  }
}

std::expected<command::CommandRecord, std::string>
PostgresStore::TransitionCommand(std::string_view command_id,
    command::CommandStatus expected, command::CommandStatus desired,
    const command::CommandUpdate& update) {
  if (!command::CanTransition(expected, desired)) {
    last_failure_kind_ = OperationFailureKind::kPermanent;
    return std::unexpected("转换 PostgreSQL 命令状态失败：状态迁移非法");
  }
  try {
    pqxx::work transaction{*connection_};
    const auto rows = transaction.exec(
        "UPDATE commands SET status = $3, error_code = $4, "
        "error_message = $5, device_ack = $6::jsonb, "
        "dispatched_at = TIMESTAMPTZ 'epoch' + $7::bigint * INTERVAL '1 microsecond', "
        "completed_at = TIMESTAMPTZ 'epoch' + $8::bigint * INTERVAL '1 microsecond', "
        "updated_at = TIMESTAMPTZ 'epoch' + $9::bigint * INTERVAL '1 microsecond' "
        "WHERE command_id = $1::uuid AND status = $2 RETURNING " +
            std::string{kCommandColumns},
        pqxx::params{
            command_id, CommandStatusText(expected), CommandStatusText(desired),
            update.error_code, update.error_message,
            update.device_ack
                ? std::optional<std::string>{update.device_ack->dump()}
                : std::nullopt,
            update.dispatched_at
                ? std::optional{ToUnixMicroseconds(*update.dispatched_at)}
                : std::nullopt,
            update.completed_at
                ? std::optional{ToUnixMicroseconds(*update.completed_at)}
                : std::nullopt,
            ToUnixMicroseconds(update.updated_at)});
    if (rows.empty()) {
      return std::unexpected("转换 PostgreSQL 命令状态失败：状态已变化");
    }
    auto changed = CommandFromRow(rows.front());
    if (!changed) return std::unexpected(changed.error());
    transaction.commit();
    return changed;
  } catch (const pqxx::broken_connection&) {
    last_failure_kind_ = ClassifyOperationFailure(IsOpen(), true);
    return std::unexpected("转换 PostgreSQL 命令状态失败");
  } catch (const std::exception&) {
    last_failure_kind_ = ClassifyOperationFailure(IsOpen(), false);
    return std::unexpected("转换 PostgreSQL 命令状态失败");
  }
}

std::expected<std::size_t, std::string>
PostgresStore::DeleteExpiredTerminalCommands(command::TimePoint before,
                                               std::size_t batch_size) {
  if (batch_size > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
    return std::unexpected("清理 PostgreSQL 命令失败：数量超出范围");
  }
  try {
    pqxx::work transaction{*connection_};
    const auto rows = transaction.exec(R"sql(
      WITH expired AS (
        SELECT command_id FROM commands
        WHERE status IN ('succeeded', 'failed', 'timeout', 'delivery_uncertain')
          AND completed_at IS NOT NULL
          AND completed_at < TIMESTAMPTZ 'epoch'
              + $1::bigint * INTERVAL '1 microsecond'
        ORDER BY completed_at, command_id
        LIMIT $2
      )
      DELETE FROM commands USING expired
      WHERE commands.command_id = expired.command_id
      RETURNING commands.command_id
    )sql", pqxx::params{ToUnixMicroseconds(before),
                         static_cast<std::int64_t>(batch_size)});
    transaction.commit();
    return rows.size();
  } catch (const pqxx::broken_connection&) {
    last_failure_kind_ = ClassifyOperationFailure(IsOpen(), true);
    return std::unexpected("清理 PostgreSQL 命令失败");
  } catch (const std::exception&) {
    last_failure_kind_ = ClassifyOperationFailure(IsOpen(), false);
    return std::unexpected("清理 PostgreSQL 命令失败");
  }
}

}  // namespace cns::postgres
