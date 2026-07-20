#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace cns::protocol {

enum class RegistrationStatus { kOnline, kOffline };

struct Registration {
  std::string vendor_id;
  RegistrationStatus status;
  std::optional<std::string> school_name;
  std::optional<std::string> dcdw_label;
};

struct Telemetry {
  nlohmann::json payload;
  std::optional<std::string> dcdw_label;
};

std::expected<Registration, std::string> ParseRegistration(
    std::string_view payload, std::string_view topic_vendor_id);
std::expected<Telemetry, std::string> ParseTelemetry(
    std::string_view payload, std::string_view topic_vendor_id);

}  // namespace cns::protocol
