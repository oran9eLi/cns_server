#include "core/runtime/device_ingress.hpp"

#include <utility>

namespace cns::runtime {

DeviceIngress::DeviceIngress(Submitter submit, DiagnosticSink diagnostic,
                             SteadyNow steady_now,
                             RateLimiter::Clock::duration window)
    : submit_(std::move(submit)), diagnostic_(std::move(diagnostic)),
      steady_now_(std::move(steady_now)), limiter_(window) {}

void DeviceIngress::Handle(mqtt::InboundMessage message) noexcept {
  const auto payload_bytes = message.payload.size();
  Submitter submit;
  {
    std::lock_guard lock(mutex_);
    if (!enabled_) return;
    submit = submit_;
  }
  bool accepted = false;
  try {
    accepted = submit && submit(std::move(message));
  } catch (...) {
    accepted = false;
  }
  if (accepted) return;

  std::lock_guard lock(mutex_);
  if (!enabled_) return;
  ++rejected_count_;
  rejected_bytes_ += payload_bytes;
  try {
    if (diagnostic_ && limiter_.ShouldEmit("mqtt_device_queue_full", steady_now_())) {
      diagnostic_("MQTT设备消息队列已满，已拒绝消息，次数=" +
                  std::to_string(rejected_count_) + "，字节数=" +
                  std::to_string(rejected_bytes_));
      rejected_count_ = 0;
      rejected_bytes_ = 0;
    }
  } catch (...) {
  }
}

void DeviceIngress::Disable() noexcept {
  std::lock_guard lock(mutex_);
  enabled_ = false;
  submit_ = {};
  diagnostic_ = {};
}

}  // namespace cns::runtime
