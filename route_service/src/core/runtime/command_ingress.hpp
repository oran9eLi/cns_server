// 本文件声明可失效的命令 MQTT 入站桥。
#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <string>

#include "core/runtime/command_service.hpp"
#include "core/runtime/rate_limiter.hpp"

namespace cns::runtime {

class CommandIngress {
 public:
  using DiagnosticSink = std::function<void(std::string)>;
  using SteadyNow = std::function<std::chrono::steady_clock::time_point()>;

  explicit CommandIngress(
      CommandService& service, std::string topic_namespace = "cns",
      DiagnosticSink diagnostic = {},
      SteadyNow steady_now = std::chrono::steady_clock::now);
  bool TryPush(mqtt::InboundMessage message) const;
  void Disable() noexcept;

 private:
  std::atomic<CommandService*> service_;
  std::string topic_namespace_;
  DiagnosticSink diagnostic_;
  SteadyNow steady_now_;
  mutable RateLimiter rejection_limiter_{std::chrono::seconds{30}};
};

}  // namespace cns::runtime
