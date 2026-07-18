// 本文件声明数据库已执行迁移结构与迁移计划纯逻辑接口。
#pragma once

#include "core/migration/migration.hpp"

#include <expected>
#include <string>
#include <vector>

namespace cns::migration {

/** 表示数据库中记录的一条已执行迁移。 */
struct AppliedMigration {
  int version;
  std::string name;
};

/** 指定仅检查迁移状态或生成执行计划。 */
enum class Mode { kCheckOnly, kApply };

/** 表示按版本顺序等待执行的迁移。 */
struct MigrationPlan {
  std::vector<Migration> pending;
};

/**
 * 校验可用迁移和已执行迁移，并生成只向前的迁移计划。
 *
 * @return 成功时返回计划；版本、名称或检查模式不满足时返回中文错误。
 */
std::expected<MigrationPlan, std::string> BuildMigrationPlan(
    const std::vector<Migration>& available,
    const std::vector<AppliedMigration>& applied, Mode mode);

}  // namespace cns::migration
