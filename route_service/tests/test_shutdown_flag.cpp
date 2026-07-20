// 本文件验证异步信号安全退出标志的请求与测试复位语义。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "core/runtime/shutdown_flag.hpp"

TEST_CASE("退出标志初始未请求") {
  cns::runtime::ShutdownFlag::ResetForTesting();

  CHECK_FALSE(cns::runtime::ShutdownFlag::Requested());
}

TEST_CASE("请求退出后标志变为已请求") {
  cns::runtime::ShutdownFlag::ResetForTesting();

  cns::runtime::ShutdownFlag::Request();

  CHECK(cns::runtime::ShutdownFlag::Requested());
}

TEST_CASE("测试复位后退出标志恢复未请求") {
  cns::runtime::ShutdownFlag::Request();

  cns::runtime::ShutdownFlag::ResetForTesting();

  CHECK_FALSE(cns::runtime::ShutdownFlag::Requested());
}
