// 本文件声明数据库迁移文件的数据结构与目录发现接口。
#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace cns::migration {

/** 表示一个经过文件名校验的可用数据库迁移。 */
struct Migration {
  int version;
  std::string name;
  std::filesystem::path file;
};

/**
 * 扫描迁移目录，解析 SQL 文件并按连续版本号排序。
 *
 * @param directory 迁移文件所在目录。
 * @return 成功时返回迁移列表，目录或文件名非法时返回中文错误。
 */
std::expected<std::vector<Migration>, std::string> DiscoverMigrations(
    const std::filesystem::path& directory);

}  // namespace cns::migration
