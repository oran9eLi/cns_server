// 本文件实现限频器的窗口边界、时间回退保护与并发互斥。
#include "core/runtime/rate_limiter.hpp"

#include <condition_variable>
#include <mutex>

extern "C" bool cns_runtime_wait_interruptibly(
    std::condition_variable* changed, std::unique_lock<std::mutex>* lock,
    std::chrono::seconds delay, const bool* stop_requested) {
  return changed->wait_for(*lock, delay,
                           [stop_requested] { return *stop_requested; });
}

namespace cns::runtime {

RateLimiter::RateLimiter(Clock::duration window) : window_(window) {}

bool RateLimiter::ShouldEmit(std::string_view key, Clock::time_point now) {
  std::lock_guard lock{mutex_};
  const auto found = last_emissions_.find(std::string{key});
  if (found != last_emissions_.end() && now - found->second < window_) {
    return false;
  }
  last_emissions_[std::string{key}] = now;
  return true;
}

}  // namespace cns::runtime
