// 本文件负责验证可用迁移与数据库已执行迁移之间的纯逻辑规划。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "core/migration/migration_plan.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace {

using cns::migration::AppliedMigration;
using cns::migration::Migration;

const std::vector<Migration> kAvailable{
    {1, "创建迁移版本表", std::filesystem::path{"001.sql"}},
    {2, "创建核心业务表", std::filesystem::path{"002.sql"}},
};

}  // namespace

TEST_CASE("数据库为空时检查模式报告首个待迁移版本") {
  const auto result = cns::migration::BuildMigrationPlan(
      kAvailable, {}, cns::migration::Mode::kCheckOnly);
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().find("001") != std::string::npos);
}

TEST_CASE("数据库为空时执行模式返回全部迁移") {
  const auto result = cns::migration::BuildMigrationPlan(
      kAvailable, {}, cns::migration::Mode::kApply);
  REQUIRE(result.has_value());
  REQUIRE(result->pending.size() == 2);
  CHECK(result->pending[0].version == 1);
  CHECK(result->pending[1].version == 2);
}

TEST_CASE("数据库与目录一致时两种模式都返回空计划") {
  const std::vector<AppliedMigration> applied{{1, "创建迁移版本表"},
                                               {2, "创建核心业务表"}};
  for (const auto mode : {cns::migration::Mode::kCheckOnly,
                          cns::migration::Mode::kApply}) {
    const auto result = cns::migration::BuildMigrationPlan(kAvailable, applied, mode);
    REQUIRE(result.has_value());
    CHECK(result->pending.empty());
  }
}

TEST_CASE("数据库缺少末尾版本时检查拒绝而执行只返回末尾版本") {
  const std::vector<AppliedMigration> applied{{1, "创建迁移版本表"}};
  const auto check = cns::migration::BuildMigrationPlan(
      kAvailable, applied, cns::migration::Mode::kCheckOnly);
  REQUIRE_FALSE(check.has_value());
  CHECK(check.error().find("002") != std::string::npos);

  const auto apply = cns::migration::BuildMigrationPlan(
      kAvailable, applied, cns::migration::Mode::kApply);
  REQUIRE(apply.has_value());
  REQUIRE(apply->pending.size() == 1);
  CHECK(apply->pending[0].version == 2);
}

TEST_CASE("数据库存在程序未知版本时两种模式都拒绝") {
  const std::vector<AppliedMigration> applied{{1, "创建迁移版本表"},
                                               {2, "创建核心业务表"},
                                               {3, "未来迁移"}};
  CHECK_FALSE(cns::migration::BuildMigrationPlan(
                  kAvailable, applied, cns::migration::Mode::kCheckOnly)
                  .has_value());
  CHECK_FALSE(cns::migration::BuildMigrationPlan(
                  kAvailable, applied, cns::migration::Mode::kApply)
                  .has_value());
}

TEST_CASE("共同版本名称不一致时拒绝") {
  const std::vector<AppliedMigration> applied{{1, "被修改的名称"}};
  CHECK_FALSE(cns::migration::BuildMigrationPlan(
                  kAvailable, applied, cns::migration::Mode::kApply)
                  .has_value());
}

TEST_CASE("数据库已执行版本不连续时拒绝") {
  const std::vector<AppliedMigration> applied{{1, "创建迁移版本表"},
                                               {3, "未来迁移"}};
  CHECK_FALSE(cns::migration::BuildMigrationPlan(
                  kAvailable, applied, cns::migration::Mode::kApply)
                  .has_value());
}

TEST_CASE("可用迁移版本不连续时拒绝") {
  const std::vector<Migration> available{
      {1, "迁移甲", std::filesystem::path{"001.sql"}},
      {3, "迁移丙", std::filesystem::path{"003.sql"}},
  };
  CHECK_FALSE(cns::migration::BuildMigrationPlan(
                  available, {}, cns::migration::Mode::kApply)
                  .has_value());
}
