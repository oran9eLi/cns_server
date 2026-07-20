#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <limits>
#include <mutex>
#include <string>

#include "adapters/mqtt/mqtt_client.hpp"
#include "core/runtime/rate_limiter.hpp"

namespace cns::runtime {

constexpr std::size_t SaturatingAdd(std::size_t left,
                                    std::size_t right) noexcept {
  const auto maximum = std::numeric_limits<std::size_t>::max();
  return right > maximum - left ? maximum : left + right;
}

/** 将 MQTT 回调非阻塞接入业务队列，并对队列拒绝提供安全限频诊断。 */
class DeviceIngress {
 public:
  using Submitter = std::function<bool(mqtt::InboundMessage)>;
  using DiagnosticSink = std::function<void(std::string)>;
  using SteadyNow = std::function<RateLimiter::Clock::time_point()>;

  DeviceIngress(Submitter submit, DiagnosticSink diagnostic,
                SteadyNow steady_now = [] { return RateLimiter::Clock::now(); },
                RateLimiter::Clock::duration window =
                    std::chrono::seconds{30});

  void Handle(mqtt::InboundMessage message) noexcept;
  void Disable() noexcept;

 private:
  std::mutex mutex_;
  Submitter submit_;
  DiagnosticSink diagnostic_;
  SteadyNow steady_now_;
  RateLimiter limiter_;
  std::size_t rejected_count_ = 0;
  std::size_t rejected_bytes_ = 0;
  bool enabled_ = true;
};

}  // namespace cns::runtime
