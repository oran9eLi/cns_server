#include "core/mqtt_topic/device_topic.hpp"

#include <algorithm>

namespace cns::mqtt_topic {

bool IsValidDeviceId(std::string_view device_id) {
  return !device_id.empty() && device_id.size() <= 64 &&
         std::ranges::all_of(device_id, [](unsigned char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'A' && character <= 'Z') ||
                  (character >= 'a' && character <= 'z') ||
                  character == '.' || character == '_' ||
                  character == ':' || character == '-';
         });
}

bool IsValidVendorId(std::string_view vendor_id) {
  return IsValidDeviceId(vendor_id);
}

std::expected<ParsedDeviceTopic, std::string> ParseDeviceTopic(
    std::string_view topic_namespace, std::string_view topic) {
  const auto first = topic.find('/');
  const auto second = first == std::string_view::npos
                          ? first
                          : topic.find('/', first + 1);
  if (first == std::string_view::npos || second == std::string_view::npos ||
      topic.find('/', second + 1) != std::string_view::npos ||
      topic.substr(0, first) != topic_namespace) {
    return std::unexpected("设备 topic 格式无效");
  }
  const auto device_id = topic.substr(first + 1, second - first - 1);
  if (!IsValidDeviceId(device_id)) return std::unexpected("device_id 无效");
  const auto kind = topic.substr(second + 1);
  if (kind == "registration") {
    return ParsedDeviceTopic{std::string{device_id},
                             DeviceMessageKind::kRegistration};
  }
  if (kind == "telemetry") {
    return ParsedDeviceTopic{std::string{device_id},
                             DeviceMessageKind::kTelemetry};
  }
  return std::unexpected("设备消息类型无效");
}

std::string RegistrationFilter(std::string_view topic_namespace) {
  return std::string{topic_namespace} + "/+/registration";
}

std::string TelemetryFilter(std::string_view topic_namespace) {
  return std::string{topic_namespace} + "/+/telemetry";
}

std::string DeviceDirectoryTopic(std::string_view topic_namespace) {
  return std::string{topic_namespace} + "/events/devices/directory";
}

std::string StateEventTopic(std::string_view topic_namespace,
                            std::string_view vendor_id) {
  return std::string{topic_namespace} + "/events/devices/" +
         std::string{vendor_id} + "/state";
}

}  // namespace cns::mqtt_topic
