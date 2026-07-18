// 本文件验证服务器日志的格式、分流、过滤、时间异常与并发完整性。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "core/logging/logger.hpp"

#include <chrono>
#include <ctime>
#include <sstream>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;

std::chrono::system_clock::time_point LocalTime(int year, int month, int day,
                                                int hour, int minute, int second,
                                                int milliseconds = 0) {
  std::tm value{};
  value.tm_year = year - 1900;
  value.tm_mon = month - 1;
  value.tm_mday = day;
  value.tm_hour = hour;
  value.tm_min = minute;
  value.tm_sec = second;
  value.tm_isdst = -1;
  return std::chrono::system_clock::from_time_t(std::mktime(&value)) +
         std::chrono::milliseconds{milliseconds};
}

cns::logging::Clock FixedClock(
    std::chrono::system_clock::time_point time_point) {
  return {[time_point] { return time_point; }};
}

}  // namespace

TEST_CASE("日志等级文本解析为强类型") {
  CHECK(cns::logging::ParseLevel("debug") == cns::logging::Level::kDebug);
  CHECK(cns::logging::ParseLevel("info") == cns::logging::Level::kInfo);
  CHECK(cns::logging::ParseLevel("warn") == cns::logging::Level::kWarn);
  CHECK(cns::logging::ParseLevel("error") == cns::logging::Level::kError);
  CHECK_FALSE(cns::logging::ParseLevel("INFO"));
}

TEST_CASE("INFO 日志使用本地时间和固定格式且不包含运行时长") {
  std::ostringstream out;
  std::ostringstream err;
  cns::logging::Logger logger{cns::logging::Level::kInfo, out, err,
                              FixedClock(LocalTime(2026, 7, 18, 14, 30, 25, 123))};

  logger.Info("服务启动");

  CHECK(out.str() == "[2026-07-18 14:30:25.123] [INFO ] 服务启动\n");
  CHECK(out.str().find("+000000.000s") == std::string::npos);
  CHECK(err.str().empty());
}

TEST_CASE("日志按等级分流到标准输出和标准错误") {
  std::ostringstream out;
  std::ostringstream err;
  cns::logging::Logger logger{cns::logging::Level::kDebug, out, err,
                              FixedClock(LocalTime(2026, 7, 18, 14, 30, 25))};

  logger.Debug("调试");
  logger.Info("信息");
  logger.Warn("警告");
  logger.Error("错误");

  CHECK(out.str().find("[DEBUG] 调试\n") != std::string::npos);
  CHECK(out.str().find("[INFO ] 信息\n") != std::string::npos);
  CHECK(err.str().find("[WARN ] 警告\n") != std::string::npos);
  CHECK(err.str().find("[ERROR] 错误\n") != std::string::npos);
}

TEST_CASE("低于最低等级的日志被过滤") {
  std::ostringstream out;
  std::ostringstream err;
  cns::logging::Logger logger{cns::logging::Level::kWarn, out, err,
                              FixedClock(LocalTime(2026, 7, 18, 14, 30, 25))};

  CHECK_FALSE(logger.Enabled(cns::logging::Level::kInfo));
  CHECK(logger.Enabled(cns::logging::Level::kWarn));
  logger.Debug("调试");
  logger.Info("信息");
  logger.Warn("警告");

  CHECK(out.str().empty());
  CHECK(err.str().find("警告") != std::string::npos);
}

TEST_CASE("消息中的回车和换行被替换为空格") {
  std::ostringstream out;
  std::ostringstream err;
  cns::logging::Logger logger{cns::logging::Level::kInfo, out, err,
                              FixedClock(LocalTime(2026, 7, 18, 14, 30, 25))};

  logger.Info("第一行\r\n第二行\n第三行");

  CHECK(out.str().find("第一行  第二行 第三行\n") != std::string::npos);
}

TEST_CASE("2025 年之前的系统时间使用固定占位符") {
  std::ostringstream out;
  std::ostringstream err;
  cns::logging::Logger logger{cns::logging::Level::kInfo, out, err,
                              FixedClock(LocalTime(2024, 12, 31, 23, 59, 59))};

  logger.Info("时间异常");

  CHECK(out.str() == "[---------- --:--:--.---] [INFO ] 时间异常\n");
}

TEST_CASE("两个线程写入的二百条日志保持完整行") {
  std::ostringstream out;
  std::ostringstream err;
  cns::logging::Logger logger{cns::logging::Level::kInfo, out, err,
                              FixedClock(LocalTime(2026, 7, 18, 14, 30, 25))};
  const auto write = [&logger](std::string prefix) {
    for (int index = 0; index < 100; ++index) {
      logger.Info(prefix + std::to_string(index));
    }
  };

  std::thread first{write, "甲-"};
  std::thread second{write, "乙-"};
  first.join();
  second.join();

  std::istringstream input{out.str()};
  std::string line;
  int count = 0;
  while (std::getline(input, line)) {
    CHECK(line.starts_with("[2026-07-18 14:30:25.000] [INFO ] "));
    CHECK((line.find("甲-") != std::string::npos ||
           line.find("乙-") != std::string::npos));
    ++count;
  }
  CHECK(count == 200);
}
