// 本文件声明基于单调时钟、按键隔离的线程安全限频器。
#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace cns::runtime {

class RateLimiter {
 public:
  using Clock = std::chrono::steady_clock;

  explicit RateLimiter(Clock::duration window);

  /** 首次调用或距该键上次放行达到窗口时返回 true。 */
  bool ShouldEmit(std::string_view key, Clock::time_point now);

 private:
  Clock::duration window_;
  std::mutex mutex_;
  std::unordered_map<std::string, Clock::time_point> last_emissions_;
};

}  // namespace cns::runtime
