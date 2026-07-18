// 本文件声明路由服务命令行选项、解析接口与中文用法文本。
#pragma once

#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace cns::cli {

struct Options {
  std::filesystem::path config_path;
  std::filesystem::path migrations_path;
  bool migrate_only = false;
  bool show_help = false;
  bool show_version = false;
};

std::expected<Options, std::string> ParseCommandLine(
    std::span<const std::string_view> arguments);
std::string_view Usage();

}  // namespace cns::cli
