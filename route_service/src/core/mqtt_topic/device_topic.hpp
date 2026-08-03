#pragma once

#include <expected>
#include <string>
#include <string_view>

namespace cns::mqtt_topic {

enum class DeviceMessageKind { kRegistration, kTelemetry };

struct ParsedDeviceTopic {
  std::string device_id;
  DeviceMessageKind kind;
};

std::expected<ParsedDeviceTopic, std::string> ParseDeviceTopic(
    std::string_view topic_namespace, std::string_view topic);
std::string RegistrationFilter(std::string_view topic_namespace);
std::string TelemetryFilter(std::string_view topic_namespace);
std::string DeviceDirectoryTopic(std::string_view topic_namespace);
std::string StateEventTopic(std::string_view topic_namespace,
                            std::string_view device_id);
bool IsValidDeviceId(std::string_view device_id);

}  // namespace cns::mqtt_topic
