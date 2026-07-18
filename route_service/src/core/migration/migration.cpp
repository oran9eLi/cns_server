// 本文件实现数据库迁移 SQL 文件的发现、显式解析与连续性校验。
#include "core/migration/migration.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <string>
#include <system_error>
#include <vector>

namespace cns::migration {
namespace {

std::expected<Migration, std::string> ParseMigration(
    const std::filesystem::directory_entry& entry) {
  const std::string filename = entry.path().filename().string();
  constexpr std::string_view suffix = ".sql";
  if (filename.size() < 8 || !filename.ends_with(suffix) || filename[3] != '_' ||
      !std::isdigit(static_cast<unsigned char>(filename[0])) ||
      !std::isdigit(static_cast<unsigned char>(filename[1])) ||
      !std::isdigit(static_cast<unsigned char>(filename[2]))) {
    return std::unexpected(std::format("迁移 SQL 文件名格式非法：{}", filename));
  }

  const int version = (filename[0] - '0') * 100 + (filename[1] - '0') * 10 +
                      (filename[2] - '0');
  if (version == 0) {
    return std::unexpected(std::format("迁移版本必须大于零：{}", filename));
  }

  const std::string name = filename.substr(4, filename.size() - 4 - suffix.size());
  if (name.empty()) {
    return std::unexpected(std::format("迁移名称不能为空：{}", filename));
  }
  if (!entry.is_regular_file()) {
    return std::unexpected(std::format("迁移 SQL 路径不是普通文件：{}", filename));
  }
  return Migration{version, name, entry.path()};
}

}  // namespace

std::expected<std::vector<Migration>, std::string> DiscoverMigrations(
    const std::filesystem::path& directory) {
  try {
    if (!std::filesystem::is_directory(directory)) {
      return std::unexpected(
          std::format("迁移目录不存在或不是目录：{}", directory.string()));
    }

    std::vector<Migration> migrations;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
      const auto filename = entry.path().filename().string();
      if (!filename.ends_with(".sql")) {
        continue;
      }
      auto migration = ParseMigration(entry);
      if (!migration) {
        return std::unexpected(std::move(migration.error()));
      }
      migrations.push_back(std::move(*migration));
    }

    std::ranges::sort(migrations, {}, &Migration::version);
    for (std::size_t index = 0; index < migrations.size(); ++index) {
      const int expected_version = static_cast<int>(index) + 1;
      if (migrations[index].version != expected_version) {
        if (index > 0 && migrations[index - 1].version == migrations[index].version) {
          return std::unexpected(
              std::format("迁移版本重复：{:03}", migrations[index].version));
        }
        return std::unexpected(std::format("迁移版本不连续，期望 {:03}，实际 {:03}",
                                           expected_version,
                                           migrations[index].version));
      }
    }
    return migrations;
  } catch (const std::filesystem::filesystem_error& error) {
    return std::unexpected(std::format("读取迁移目录失败：{}", error.what()));
  } catch (const std::system_error& error) {
    return std::unexpected(std::format("读取迁移目录失败：{}", error.what()));
  }
}

}  // namespace cns::migration
