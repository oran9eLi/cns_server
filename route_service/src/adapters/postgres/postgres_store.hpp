// 本文件声明 PostgreSQL 连接与数据库迁移执行适配层。
#pragma once

#include "core/config/app_config.hpp"
#include "core/device/device_registry.hpp"
#include "core/logging/logger.hpp"
#include "core/migration/migration_plan.hpp"
#include "core/persistence/dirty_state.hpp"
#include "core/protocol/device_message.hpp"

#include <chrono>
#include <expected>
#include <memory>
#include <string>
#include <vector>

namespace pqxx {
class connection;
}

namespace cns::postgres {

struct ProvisionRequest {
  protocol::Registration registration;
  std::chrono::system_clock::time_point received_at;
};

struct DeviceWritePlan {
  bool update_metadata;
  bool update_latest_state;

  bool operator==(const DeviceWritePlan&) const = default;
};

DeviceWritePlan PlanDeviceWrite(bool write_metadata, bool write_status,
                                bool write_telemetry) noexcept;
std::expected<void, std::string> ValidateDeviceWrite(
    const persistence::DesiredDeviceWrite& write);
std::expected<std::optional<nlohmann::json>, std::string> ParseDeviceTelemetry(
    const std::optional<std::string>& text);
std::int64_t ToUnixMicroseconds(device::TimePoint time) noexcept;
std::expected<device::TimePoint, std::string> FromUnixMicroseconds(
    std::int64_t microseconds) noexcept;

/** 构造不包含密码和完整连接串的安全连接描述。 */
std::string BuildSafeConnectionDescription(const config::DatabaseConfig& config);

/** 构造已按 libpq conninfo 规则转义的完整连接参数。 */
std::string BuildConnectionString(const config::DatabaseConfig& config);

/** 独占一个同步 PostgreSQL 连接并提供迁移所需的最小操作。 */
class PostgresStore {
 public:
  ~PostgresStore();

  PostgresStore(const PostgresStore&) = delete;
  PostgresStore& operator=(const PostgresStore&) = delete;

  static std::expected<std::unique_ptr<PostgresStore>, std::string> Connect(
      const config::DatabaseConfig& config, logging::Logger& logger);

  /** 读取数据库中按版本升序排列的已执行迁移；表不存在时返回空列表。 */
  std::expected<std::vector<migration::AppliedMigration>, std::string>
  ReadAppliedMigrations();

  /** 在一个事务内执行迁移 SQL 并记录对应版本。 */
  std::expected<void, std::string> ApplyMigration(
      const migration::Migration& migration);

  std::expected<std::vector<device::DeviceRecord>, std::string> LoadDevices();
  std::expected<device::DeviceRecord, std::string> ProvisionDevice(
      const ProvisionRequest& request);
  std::expected<void, std::string> WriteDeviceState(
      const persistence::DesiredDeviceWrite& write);
  bool IsOpen() const noexcept;

 private:
  explicit PostgresStore(std::unique_ptr<pqxx::connection> connection,
                         logging::Logger& logger);

  std::unique_ptr<pqxx::connection> connection_;
  logging::Logger& logger_;
};

}  // namespace cns::postgres
