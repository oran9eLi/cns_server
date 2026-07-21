// 本文件实现命令 MQTT 入站桥的失效语义。
#include "core/runtime/command_ingress.hpp"

namespace cns::runtime {

CommandIngress::CommandIngress(CommandService& service,
                               std::string topic_namespace,
                               DiagnosticSink diagnostic,
                               SteadyNow steady_now)
    : service_(&service),
      topic_namespace_(std::move(topic_namespace)),
      diagnostic_(std::move(diagnostic)),
      steady_now_(std::move(steady_now)) {}

bool CommandIngress::TryPush(mqtt::InboundMessage message) const {
  if (!command::ParseSourceRequestTopic(topic_namespace_, message.topic) &&
      !command::ParseDeviceConfigAckTopic(topic_namespace_, message.topic) &&
      !command::ParseSourceControlRequestTopic(topic_namespace_, message.topic) &&
      !command::ParseDeviceControlAckTopic(topic_namespace_, message.topic)) {
    return false;
  }
  auto* service = service_.load(std::memory_order_acquire);
  const bool pushed = service != nullptr && service->TryPush(std::move(message));
  if (!pushed && diagnostic_ &&
      rejection_limiter_.ShouldEmit("command_ingress_rejected", steady_now_())) {
    try {
      diagnostic_("命令MQTT入站队列已关闭或容量已满");
    } catch (...) {
    }
  }
  return pushed;
}

void CommandIngress::Disable() noexcept {
  service_.store(nullptr, std::memory_order_release);
}

}  // namespace cns::runtime
