// 本文件声明服务器中文分级日志的强类型等级与线程安全输出接口。
#pragma once

#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <ostream>
#include <string_view>

namespace cns::logging {

enum class Level { kDebug = 0, kInfo = 1, kWarn = 2, kError = 3 };

std::optional<Level> ParseLevel(std::string_view text);

struct Clock {
  std::function<std::chrono::system_clock::time_point()> system_now;
};

class Logger {
 public:
  Logger(Level minimum_level, std::ostream& out, std::ostream& err,
         Clock clock = {});

  bool Enabled(Level level) const;
  void Debug(std::string_view message);
  void Info(std::string_view message);
  void Warn(std::string_view message);
  void Error(std::string_view message);

 private:
  void Write(Level level, std::string_view message);

  Level minimum_level_;
  std::ostream& out_;
  std::ostream& err_;
  Clock clock_;
  std::mutex mutex_;
};

}  // namespace cns::logging
