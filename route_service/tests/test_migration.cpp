// 本文件负责验证迁移 SQL 文件的发现、解析、排序与目录约束。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "core/migration/migration.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

class TempDirectory {
 public:
  TempDirectory()
      : path_(std::filesystem::temp_directory_path() /
              ("cns_migration_test_" +
               std::to_string(std::chrono::steady_clock::now()
                                  .time_since_epoch()
                                  .count()))) {
    std::filesystem::create_directory(path_);
  }

  ~TempDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path& path() const { return path_; }

  void Touch(const std::filesystem::path& name) const {
    std::ofstream file(path_ / name);
    REQUIRE(file.good());
  }

 private:
  std::filesystem::path path_;
};

}  // namespace

TEST_CASE("中文迁移文件名被解析并按版本排序") {
  const TempDirectory directory;
  directory.Touch("002_创建核心业务表.sql");
  directory.Touch("001_创建迁移版本表.sql");
  directory.Touch("README.md");

  const auto result = cns::migration::DiscoverMigrations(directory.path());
  REQUIRE(result.has_value());
  REQUIRE(result->size() == 2);
  CHECK((*result)[0].version == 1);
  CHECK((*result)[0].name == "创建迁移版本表");
  CHECK((*result)[0].file.filename() == "001_创建迁移版本表.sql");
  CHECK((*result)[1].version == 2);
  CHECK((*result)[1].name == "创建核心业务表");
}

TEST_CASE("格式错误的 SQL 文件名被拒绝") {
  const TempDirectory directory;
  directory.Touch("1_错误格式.sql");
  CHECK_FALSE(cns::migration::DiscoverMigrations(directory.path()).has_value());
}

TEST_CASE("版本零被拒绝") {
  const TempDirectory directory;
  directory.Touch("000_非法版本.sql");
  CHECK_FALSE(cns::migration::DiscoverMigrations(directory.path()).has_value());
}

TEST_CASE("重复版本被拒绝") {
  const TempDirectory directory;
  directory.Touch("001_迁移甲.sql");
  directory.Touch("001_迁移乙.sql");
  CHECK_FALSE(cns::migration::DiscoverMigrations(directory.path()).has_value());
}

TEST_CASE("首个版本不是 001 被拒绝") {
  const TempDirectory directory;
  directory.Touch("002_迁移.sql");
  CHECK_FALSE(cns::migration::DiscoverMigrations(directory.path()).has_value());
}

TEST_CASE("迁移版本存在缺口被拒绝") {
  const TempDirectory directory;
  directory.Touch("001_迁移甲.sql");
  directory.Touch("003_迁移丙.sql");
  CHECK_FALSE(cns::migration::DiscoverMigrations(directory.path()).has_value());
}

TEST_CASE("迁移名称为空被拒绝") {
  const TempDirectory directory;
  directory.Touch("001_.sql");
  CHECK_FALSE(cns::migration::DiscoverMigrations(directory.path()).has_value());
}

TEST_CASE("迁移目录不存在或不是目录时被拒绝") {
  const TempDirectory directory;
  const auto missing = directory.path() / "不存在";
  CHECK_FALSE(cns::migration::DiscoverMigrations(missing).has_value());

  directory.Touch("普通文件");
  CHECK_FALSE(
      cns::migration::DiscoverMigrations(directory.path() / "普通文件").has_value());
}
