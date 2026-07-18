// 本文件实现服务器日志的本地时间格式化、分级过滤、标准流分流与整行互斥输出。
#include "core/logging/logger.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>

namespace cns::logging {
namespace {

std::string_view LevelName(Level level) {
  switch (level) {
    case Level::kDebug:
      return "DEBUG";
    case Level::kInfo:
      return "INFO ";
    case Level::kWarn:
      return "WARN ";
    case Level::kError:
      return "ERROR";
  }
  return "ERROR";
}

std::string FormatTime(std::chrono::system_clock::time_point now) {
  const auto seconds = std::chrono::floor<std::chrono::seconds>(now);
  const auto milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - seconds).count();
  const std::time_t raw = std::chrono::system_clock::to_time_t(seconds);
  std::tm local{};
  if (localtime_r(&raw, &local) == nullptr || local.tm_year + 1900 < 2025) {
    return "---------- --:--:--.---";
  }

  std::ostringstream text;
  text << std::put_time(&local, "%Y-%m-%d %H:%M:%S") << '.' << std::setw(3)
       << std::setfill('0') << milliseconds;
  return text.str();
}

std::string SingleLine(std::string_view message) {
  std::string result{message};
  for (char& character : result) {
    if (character == '\r' || character == '\n') character = ' ';
  }
  return result;
}

}  // namespace

std::optional<Level> ParseLevel(std::string_view text) {
  if (text == "debug") return Level::kDebug;
  if (text == "info") return Level::kInfo;
  if (text == "warn") return Level::kWarn;
  if (text == "error") return Level::kError;
  return std::nullopt;
}

Logger::Logger(Level minimum_level, std::ostream& out, std::ostream& err,
               Clock clock)
    : minimum_level_(minimum_level), out_(out), err_(err), clock_(std::move(clock)) {
  if (!clock_.system_now) {
    clock_.system_now = [] { return std::chrono::system_clock::now(); };
  }
}

bool Logger::Enabled(Level level) const { return level >= minimum_level_; }

void Logger::Debug(std::string_view message) { Write(Level::kDebug, message); }
void Logger::Info(std::string_view message) { Write(Level::kInfo, message); }
void Logger::Warn(std::string_view message) { Write(Level::kWarn, message); }
void Logger::Error(std::string_view message) { Write(Level::kError, message); }

void Logger::Write(Level level, std::string_view message) {
  if (!Enabled(level)) return;
  std::lock_guard lock{mutex_};
  std::ostream& stream = level >= Level::kWarn ? err_ : out_;
  stream << '[' << FormatTime(clock_.system_now()) << "] [" << LevelName(level)
         << "] " << SingleLine(message) << '\n';
  stream.flush();
}

}  // namespace cns::logging
