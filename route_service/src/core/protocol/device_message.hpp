#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace cns::protocol {

enum class RegistrationStatus { kOnline, kOffline };
enum class DeviceType { kCnsBox, kFlightController };

std::string_view ToString(DeviceType type);

struct Registration {
  std::string vendor_id;
  RegistrationStatus status;
  std::optional<std::string> school_name;
  std::optional<std::string> dcdw_label;
  DeviceType device_type = DeviceType::kCnsBox;
};

struct Telemetry {
  nlohmann::json payload;
  std::optional<std::string> dcdw_label;
  std::optional<DeviceType> device_type;

  Telemetry(nlohmann::json payload_value,
            std::optional<std::string> dcdw_label_value,
            std::optional<DeviceType> device_type_value = std::nullopt)
      : payload(std::move(payload_value)),
        dcdw_label(std::move(dcdw_label_value)),
        device_type(device_type_value) {}
};

std::expected<Registration, std::string> ParseRegistration(
    std::string_view payload, std::string_view topic_vendor_id);
std::expected<Telemetry, std::string> ParseTelemetry(
    std::string_view payload, std::string_view topic_vendor_id);

}  // namespace cns::protocol
