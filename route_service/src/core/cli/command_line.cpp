// 本文件负责纯内存解析命令行参数并校验运行模式之间的组合约束。
#include "core/cli/command_line.hpp"

#include <utility>

namespace cns::cli {
namespace {

bool LooksLikeOption(const std::string_view value) {
  return value.starts_with("--");
}

std::expected<std::string_view, std::string> ReadPathValue(
    const std::span<const std::string_view> arguments, std::size_t& index,
    const std::string_view option) {
  if (index + 1 >= arguments.size() || arguments[index + 1].empty() ||
      LooksLikeOption(arguments[index + 1])) {
    return std::unexpected(std::string(option) + " 缺少非空路径值");
  }
  ++index;
  return arguments[index];
}

}  // namespace

std::expected<Options, std::string> ParseCommandLine(
    const std::span<const std::string_view> arguments) {
  Options options;
  bool has_config = false;
  bool has_migrations = false;
  bool has_migrate_only = false;
  bool has_help = false;
  bool has_version = false;

  for (std::size_t index = arguments.empty() ? 0 : 1; index < arguments.size(); ++index) {
    const auto argument = arguments[index];
    if (argument == "--config") {
      if (has_config) {
        return std::unexpected("重复参数：--config");
      }
      has_config = true;
      auto value = ReadPathValue(arguments, index, argument);
      if (!value) {
        return std::unexpected(std::move(value.error()));
      }
      options.config_path = *value;
    } else if (argument == "--migrations") {
      if (has_migrations) {
        return std::unexpected("重复参数：--migrations");
      }
      has_migrations = true;
      auto value = ReadPathValue(arguments, index, argument);
      if (!value) {
        return std::unexpected(std::move(value.error()));
      }
      options.migrations_path = *value;
    } else if (argument == "--migrate-only") {
      if (has_migrate_only) {
        return std::unexpected("重复参数：--migrate-only");
      }
      has_migrate_only = true;
      options.migrate_only = true;
    } else if (argument == "--help") {
      if (has_help) {
        return std::unexpected("重复参数：--help");
      }
      has_help = true;
      options.show_help = true;
    } else if (argument == "--version") {
      if (has_version) {
        return std::unexpected("重复参数：--version");
      }
      has_version = true;
      options.show_version = true;
    } else {
      return std::unexpected("未知参数：" + std::string(argument));
    }
  }

  const auto option_count = arguments.empty() ? 0U : arguments.size() - 1U;
  if ((has_help || has_version) && option_count != 1U) {
    return std::unexpected("--help 和 --version 只能单独使用");
  }
  if (has_help || has_version) {
    return options;
  }
  if (!has_config) {
    return std::unexpected("缺少必需参数：--config");
  }
  if (!has_migrations) {
    return std::unexpected("缺少必需参数：--migrations");
  }
  return options;
}

std::string_view Usage() {
  return R"(用法：
  route_service --config <配置文件路径> --migrations <迁移目录> [--migrate-only]
  route_service --help
  route_service --version
)";
}

}  // namespace cns::cli
