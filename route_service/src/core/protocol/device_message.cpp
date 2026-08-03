#include "core/protocol/device_message.hpp"

#include <array>

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

std::expected<void, std::string> ValidateOptionalStringArray(
    const nlohmann::json& object, std::string_view key) {
  const auto iterator = object.find(key);
  if (iterator == object.end()) return {};
  if (!iterator->is_array()) {
    return std::unexpected(std::string{key} + " 必须是字符串数组");
  }
  for (const auto& value : *iterator) {
    if (!value.is_string() || value.get_ref<const std::string&>().empty()) {
      return std::unexpected(std::string{key} + " 必须是字符串数组");
    }
  }
  return {};
}

std::expected<void, std::string> ValidateOptionalObject(
    const nlohmann::json& object, std::string_view key) {
  const auto iterator = object.find(key);
  if (iterator != object.end() && !iterator->is_object()) {
    return std::unexpected(std::string{key} + " 必须是 object");
  }
  return {};
}

std::expected<void, std::string> RejectRemovedFields(
    const nlohmann::json& object) {
  constexpr std::array<std::string_view, 8> removed{
      "vendor_id", "remote_id", "uid",       "uid2",
      "gateway_id", "rpi_serial", "endpoint", "identity"};
  for (const auto field : removed) {
    if (object.contains(field)) {
      return std::unexpected(std::string{field} + " 已从 schema v3 删除");
    }
  }
  return {};
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
    std::string_view payload, std::string_view topic_device_id) {
  auto root = ParseObject(payload);
  if (!root) return std::unexpected(root.error());
  const auto version = root->find("schema_version");
  if (version == root->end() || !version->is_number_integer() ||
      *version != 3) {
    return std::unexpected("schema_version 必须是整数 3");
  }
  if (auto clean = RejectRemovedFields(*root); !clean) {
    return std::unexpected(clean.error());
  }
  auto device_id = RequiredString(*root, "device_id");
  if (!device_id) return std::unexpected(device_id.error());
  if (auto valid = ValidateDeviceId(*device_id, topic_device_id); !valid) {
    return std::unexpected(valid.error());
  }
  auto device_type = ParseDeviceType(*root);
  if (!device_type) return std::unexpected(device_type.error());
  auto status = RequiredString(*root, "status");
  if (!status) return std::unexpected(status.error());
  if (*status != "online" && *status != "offline") {
    return std::unexpected("status 无效");
  }

  auto school = OptionalString(*root, "school_name");
  if (!school) return std::unexpected(school.error());
  auto label = OptionalString(*root, "dcdw_label");
  if (!label) return std::unexpected(label.error());
  if (auto valid = ValidateOptionalStringArray(*root, "capabilities"); !valid) {
    return std::unexpected(valid.error());
  }
  if (auto valid = ValidateOptionalObject(*root, "product"); !valid) {
    return std::unexpected(valid.error());
  }
  if (auto valid = ValidateOptionalObject(*root, "version"); !valid) {
    return std::unexpected(valid.error());
  }
  if (*status == "online" && *device_type == DeviceType::kCnsBox && !*school) {
    return std::unexpected("cns_box online registration 缺少 school_name");
  }
  std::optional<std::vector<std::string>> capabilities;
  if (const auto iterator = root->find("capabilities"); iterator != root->end()) {
    capabilities = iterator->get<std::vector<std::string>>();
  }
  std::optional<nlohmann::json> product;
  if (const auto iterator = root->find("product"); iterator != root->end()) {
    product = *iterator;
  }
  std::optional<nlohmann::json> product_version;
  if (const auto iterator = root->find("version"); iterator != root->end()) {
    product_version = *iterator;
  }
  return Registration{
      .device_id = std::move(*device_id),
      .status = *status == "online" ? RegistrationStatus::kOnline
                                    : RegistrationStatus::kOffline,
      .school_name = std::move(*school),
      .dcdw_label = std::move(*label),
      .device_type = *device_type,
      .capabilities = std::move(capabilities),
      .product = std::move(product),
      .version = std::move(product_version),
  };
}

std::expected<Telemetry, std::string> ParseTelemetry(
    std::string_view payload, std::string_view topic_device_id) {
  auto root = ParseObject(payload);
  if (!root) return std::unexpected(root.error());
  const auto version = root->find("schema_version");
  if (version == root->end() || !version->is_number_integer() ||
      *version != 3) {
    return std::unexpected("telemetry schema_version 必须是整数 3");
  }
  if (auto clean = RejectRemovedFields(*root); !clean) {
    return std::unexpected(clean.error());
  }
  auto device_id = RequiredString(*root, "device_id");
  if (!device_id) return std::unexpected(device_id.error());
  if (auto valid = ValidateDeviceId(*device_id, topic_device_id); !valid) {
    return std::unexpected(valid.error());
  }
  auto device_type = ParseDeviceType(*root);
  if (!device_type) return std::unexpected(device_type.error());
  if (auto sent_at = RequiredString(*root, "sent_at"); !sent_at) {
    return std::unexpected(sent_at.error());
  }
  const auto telemetry = root->find("telemetry");
  if (telemetry == root->end() || !telemetry->is_object()) {
    return std::unexpected("telemetry 必须是 object");
  }
  const auto drone_id = root->find("drone_id");
  if (drone_id == root->end() || !drone_id->is_object()) {
    return std::unexpected("drone_id 必须是 object");
  }
  const auto basic_id = drone_id->find("basic_id");
  if (basic_id == drone_id->end() || !basic_id->is_object()) {
    return std::unexpected("drone_id.basic_id 必须是 object");
  }
  if (basic_id->contains("uas_id")) {
    return std::unexpected("drone_id.basic_id.uas_id 已从 schema v3 删除");
  }
  return Telemetry{std::move(*root), std::nullopt, *device_type};
}

}  // namespace cns::protocol
