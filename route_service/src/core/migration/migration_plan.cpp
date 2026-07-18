// 本文件实现迁移列表连续性、共同前缀一致性与待执行计划判断。
#include "core/migration/migration_plan.hpp"

#include <algorithm>
#include <expected>
#include <format>
#include <string>
#include <vector>

namespace cns::migration {
namespace {

template <typename MigrationType>
std::expected<void, std::string> ValidateContinuous(
    const std::vector<MigrationType>& migrations, std::string_view source) {
  for (std::size_t index = 0; index < migrations.size(); ++index) {
    const int expected_version = static_cast<int>(index) + 1;
    if (migrations[index].version != expected_version) {
      return std::unexpected(std::format(
          "{}迁移版本不连续，期望 {:03}，实际 {:03}", source, expected_version,
          migrations[index].version));
    }
  }
  return {};
}

}  // namespace

std::expected<MigrationPlan, std::string> BuildMigrationPlan(
    const std::vector<Migration>& available,
    const std::vector<AppliedMigration>& applied, Mode mode) {
  if (auto result = ValidateContinuous(available, "目录"); !result) {
    return std::unexpected(std::move(result.error()));
  }
  if (auto result = ValidateContinuous(applied, "数据库"); !result) {
    return std::unexpected(std::move(result.error()));
  }

  const std::size_t common_size = std::min(available.size(), applied.size());
  for (std::size_t index = 0; index < common_size; ++index) {
    if (available[index].version != applied[index].version ||
        available[index].name != applied[index].name) {
      return std::unexpected(std::format(
          "迁移 {:03} 的目录名称与数据库记录不一致", available[index].version));
    }
  }
  if (applied.size() > available.size()) {
    return std::unexpected(std::format("数据库包含程序未知的迁移版本：{:03}",
                                       applied[available.size()].version));
  }
  if (applied.size() == available.size()) {
    return MigrationPlan{};
  }
  if (mode == Mode::kCheckOnly) {
    return std::unexpected(std::format("存在待执行迁移 {:03}，请显式运行迁移模式",
                                       available[applied.size()].version));
  }

  return MigrationPlan{std::vector<Migration>(available.begin() +
                                                  static_cast<std::ptrdiff_t>(applied.size()),
                                              available.end())};
}

}  // namespace cns::migration
