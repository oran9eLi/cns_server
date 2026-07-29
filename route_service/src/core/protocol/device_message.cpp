#include "core/protocol/device_message.hpp"

#include "core/mqtt_topic/device_topic.hpp"

namespace cns::protocol {
namespace {

std::expected<nlohmann::json, std::string> ParseObject(std::string_view payload) {
  auto value = nlohmann::json::parse(payload, nullptr, false);
  if (value.is_discarded()) return std::unexpected("JSON 语法无效");
  if (!value.is_object()) return std::unexpected("JSON 根节点必须是 object");
  return value;
}

std::expected<std::string, std::string> RequiredString(
    const nlohmann::json& object, std::string_view key) {
  const auto iterator = object.find(key);
  if (iterator == object.end() || !iterator->is_string() ||
      iterator->get_ref<const std::string&>().empty()) {
    return std::unexpected(std::string{key} + " 必须是非空字符串");
  }
  return iterator->get<std::string>();
}

std::expected<std::optional<std::string>, std::string> OptionalString(
    const nlohmann::json& object, std::string_view key) {
  const auto iterator = object.find(key);
  if (iterator == object.end() || iterator->is_null()) {
    return std::optional<std::string>{};
  }
  if (!iterator->is_string() || iterator->get_ref<const std::string&>().empty()) {
    return std::unexpected(std::string{key} + " 必须是非空字符串");
  }
  return std::optional<std::string>{iterator->get<std::string>()};
}

std::expected<DeviceType, std::string> ParseDeviceType(
    const nlohmann::json& object) {
  auto value = RequiredString(object, "device_type");
  if (!value) return std::unexpected(value.error());
  if (*value == "cns_box") return DeviceType::kCnsBox;
  if (*value == "flight_controller") return DeviceType::kFlightController;
  return std::unexpected("device_type 无效");
}

std::expected<void, std::string> ValidateDeviceId(
    std::string_view device_id, std::string_view topic_device_id) {
  if (!mqtt_topic::IsValidDeviceId(device_id) ||
      device_id != topic_device_id) {
    return std::unexpected("device_id 与 topic 不一致");
  }
  return {};
}

}  // namespace

std::string_view ToString(DeviceType type) {
  return type == DeviceType::kCnsBox ? "cns_box" : "flight_controller";
}

std::expected<Registration, std::string> ParseRegistration(
    std::string_view payload, std::string_view topic_vendor_id) {
  auto root = ParseObject(payload);
  if (!root) return std::unexpected(root.error());
  const auto version = root->find("schema_version");
  if (version == root->end() || !version->is_number_integer()) {
    return std::unexpected("schema_version 必须是整数 1 或 2");
  }

  if (*version == 1) {
    auto vendor = RequiredString(*root, "vendor_id");
    if (!vendor) return std::unexpected(vendor.error());
    if (auto valid = ValidateDeviceId(*vendor, topic_vendor_id); !valid) {
      return std::unexpected("vendor_id 与 topic 不一致");
    }
    auto status = RequiredString(*root, "status");
    if (!status) return std::unexpected(status.error());
    if (*status != "online" && *status != "offline") {
      return std::unexpected("status 无效");
    }
    auto school = OptionalString(*root, "school_name");
    if (!school) return std::unexpected(school.error());
    auto label = OptionalString(*root, "dcdw_label");
    if (!label) return std::unexpected(label.error());
    if (*status == "online" && !*school) {
      return std::unexpected("online registration 缺少 school_name");
    }
    return Registration{
        .vendor_id = std::move(*vendor),
        .status = *status == "online" ? RegistrationStatus::kOnline
                                      : RegistrationStatus::kOffline,
        .school_name = std::move(*school),
        .dcdw_label = std::move(*label),
        .device_type = DeviceType::kCnsBox,
    };
  }

  if (*version != 2) {
    return std::unexpected("schema_version 必须是整数 1 或 2");
  }
  auto device_id = RequiredString(*root, "device_id");
  if (!device_id) return std::unexpected(device_id.error());
  if (auto valid = ValidateDeviceId(*device_id, topic_vendor_id); !valid) {
    return std::unexpected(valid.error());
  }
  auto device_type = ParseDeviceType(*root);
  if (!device_type) return std::unexpected(device_type.error());
  auto status = RequiredString(*root, "status");
  if (!status) return std::unexpected(status.error());
  if (*status != "online" && *status != "offline") {
    return std::unexpected("status 无效");
  }

  std::optional<std::string> school;
  std::optional<std::string> label;
  const auto identity = root->find("identity");
  if (identity != root->end() && !identity->is_null()) {
    if (!identity->is_object()) return std::unexpected("identity 必须是 object");
    auto parsed_school = OptionalString(*identity, "school_name");
    if (!parsed_school) return std::unexpected(parsed_school.error());
    school = std::move(*parsed_school);
    auto parsed_label = OptionalString(*identity, "dcdw_label");
    if (!parsed_label) return std::unexpected(parsed_label.error());
    label = std::move(*parsed_label);
    const auto vendor = identity->find("vendor_id");
    if (vendor != identity->end() && !vendor->is_null()) {
      if (*device_type != DeviceType::kCnsBox || !vendor->is_string() ||
          vendor->get_ref<const std::string&>() != *device_id) {
        return std::unexpected("identity.vendor_id 与 device_id 不一致");
      }
    }
  }
  if (*status == "online" && *device_type == DeviceType::kCnsBox && !school) {
    return std::unexpected("cns_box online registration 缺少 school_name");
  }
  return Registration{
      .vendor_id = std::move(*device_id),
      .status = *status == "online" ? RegistrationStatus::kOnline
                                    : RegistrationStatus::kOffline,
      .school_name = std::move(school),
      .dcdw_label = std::move(label),
      .device_type = *device_type,
  };
}

std::expected<Telemetry, std::string> ParseTelemetry(
    std::string_view payload, std::string_view topic_vendor_id) {
  auto root = ParseObject(payload);
  if (!root) return std::unexpected(root.error());

  std::optional<DeviceType> device_type;
  const auto version = root->find("schema_version");
  if (version != root->end()) {
    if (!version->is_number_integer() || (*version != 1 && *version != 2)) {
      return std::unexpected("telemetry schema_version 必须是整数 1 或 2");
    }
    if (*version == 2) {
      auto device_id = RequiredString(*root, "device_id");
      if (!device_id) return std::unexpected(device_id.error());
      if (auto valid = ValidateDeviceId(*device_id, topic_vendor_id); !valid) {
        return std::unexpected(valid.error());
      }
      auto parsed_type = ParseDeviceType(*root);
      if (!parsed_type) return std::unexpected(parsed_type.error());
      device_type = *parsed_type;
    }
  }

  std::optional<std::string> label;
  const auto identity = root->find("identity");
  if (identity != root->end()) {
    if (!identity->is_object()) return std::unexpected("identity 必须是 object");
    const auto vendor = identity->find("vendor_id");
    if (vendor != identity->end() && !vendor->is_null()) {
      if (!vendor->is_string() ||
          vendor->get_ref<const std::string&>() != topic_vendor_id) {
        return std::unexpected("identity.vendor_id 与 topic 不一致");
      }
    }
    auto parsed_label = OptionalString(*identity, "dcdw_label");
    if (!parsed_label) return std::unexpected(parsed_label.error());
    label = std::move(*parsed_label);
  }
  return Telemetry{std::move(*root), std::move(label), device_type};
}

}  // namespace cns::protocol
