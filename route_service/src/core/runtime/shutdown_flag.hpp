// 本文件提供信号处理函数可安全设置的进程退出标志。
#pragma once

#include <csignal>

namespace cns::runtime {

/** 保存只由信号处理函数写入的异步信号安全退出状态。 */
class ShutdownFlag {
 public:
  static void Request() noexcept { requested_ = 1; }

  static bool Requested() noexcept { return requested_ != 0; }

  static void ResetForTesting() noexcept { requested_ = 0; }

 private:
  inline static volatile std::sig_atomic_t requested_ = 0;
};

}  // namespace cns::runtime
