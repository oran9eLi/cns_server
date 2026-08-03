#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace cns::protocol {

enum class RegistrationStatus { kOnline, kOffline };
enum class DeviceType { kCnsBox, kFlightController };

std::string_view ToString(DeviceType type);

struct Registration {
  std::string device_id;
  RegistrationStatus status;
  std::optional<std::string> school_name = std::nullopt;
  std::optional<std::string> dcdw_label = std::nullopt;
  DeviceType device_type = DeviceType::kCnsBox;
  std::optional<std::vector<std::string>> capabilities = std::nullopt;
  std::optional<nlohmann::json> product = std::nullopt;
  std::optional<nlohmann::json> version = std::nullopt;
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
    std::string_view payload, std::string_view topic_device_id);
std::expected<Telemetry, std::string> ParseTelemetry(
    std::string_view payload, std::string_view topic_device_id);

}  // namespace cns::protocol
