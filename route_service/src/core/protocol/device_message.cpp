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
  if (iterator == object.end()) return std::optional<std::string>{};
  if (!iterator->is_string() || iterator->get_ref<const std::string&>().empty()) {
    return std::unexpected(std::string{key} + " 必须是非空字符串");
  }
  return std::optional<std::string>{iterator->get<std::string>()};
}

}  // namespace

std::expected<Registration, std::string> ParseRegistration(
    std::string_view payload, std::string_view topic_vendor_id) {
  auto root = ParseObject(payload);
  if (!root) return std::unexpected(root.error());
  const auto version = root->find("schema_version");
  if (version == root->end() || !version->is_number_integer() || *version != 1) {
    return std::unexpected("schema_version 必须是整数 1");
  }
  auto vendor = RequiredString(*root, "vendor_id");
  if (!vendor) return std::unexpected(vendor.error());
  if (!mqtt_topic::IsValidVendorId(*vendor) || *vendor != topic_vendor_id) {
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
  return Registration{std::move(*vendor),
                      *status == "online" ? RegistrationStatus::kOnline
                                            : RegistrationStatus::kOffline,
                      std::move(*school), std::move(*label)};
}

std::expected<Telemetry, std::string> ParseTelemetry(
    std::string_view payload, std::string_view topic_vendor_id) {
  auto root = ParseObject(payload);
  if (!root) return std::unexpected(root.error());
  std::optional<std::string> label;
  const auto identity = root->find("identity");
  if (identity != root->end()) {
    if (!identity->is_object()) return std::unexpected("identity 必须是 object");
    const auto vendor = identity->find("vendor_id");
    if (vendor != identity->end()) {
      if (!vendor->is_string() || vendor->get_ref<const std::string&>() != topic_vendor_id) {
        return std::unexpected("identity.vendor_id 与 topic 不一致");
      }
    }
    auto parsed_label = OptionalString(*identity, "dcdw_label");
    if (!parsed_label) return std::unexpected(parsed_label.error());
    label = std::move(*parsed_label);
  }
  return Telemetry{std::move(*root), std::move(label)};
}

}  // namespace cns::protocol
