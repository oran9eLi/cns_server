#include "core/device/device_registry.hpp"

#include <algorithm>
#include <functional>
#include <type_traits>
#include <utility>

namespace cns::device {
namespace {

std::optional<std::string> AppendDiagnostic(
    std::optional<std::string> diagnostic, std::string message) {
  if (!diagnostic) return message;
  *diagnostic += "；" + message;
  return diagnostic;
}

}  // namespace

static_assert(std::is_nothrow_swappable_v<DeviceRecord>);

std::size_t DeviceRegistry::RoleKeyHash::operator()(
    const RoleKey& key) const noexcept {
  const auto school_hash = std::hash<std::int64_t>{}(key.school_id);
  const auto label_hash = std::hash<std::string>{}(key.dcdw_label);
  return school_hash ^ (label_hash + 0x9e3779b9U + (school_hash << 6U) +
                        (school_hash >> 2U));
}

std::size_t DeviceRegistry::NamedRoleKeyHash::operator()(
    const NamedRoleKey& key) const noexcept {
  const auto school_hash = std::hash<std::string>{}(key.school_name);
  const auto label_hash = std::hash<std::string>{}(key.dcdw_label);
  return school_hash ^ (label_hash + 0x9e3779b9U + (school_hash << 6U) +
                        (school_hash >> 2U));
}

void DeviceRegistry::AddNamedRole(const DeviceRecord& record) {
  if (!record.dcdw_label) return;
  named_roles_[NamedRoleKey{record.school_name, *record.dcdw_label}]
      .push_back(record.vendor_id);
}

void DeviceRegistry::RemoveNamedRole(const DeviceRecord& record) {
  if (!record.dcdw_label) return;
  const NamedRoleKey key{record.school_name, *record.dcdw_label};
  const auto iterator = named_roles_.find(key);
  if (iterator == named_roles_.end()) return;
  auto& vendors = iterator->second;
  std::erase(vendors, record.vendor_id);
  if (vendors.empty()) named_roles_.erase(iterator);
}

std::expected<void, std::string> DeviceRegistry::Load(
    std::vector<DeviceRecord> records) {
  std::unordered_map<std::string, DeviceRecord> new_records;
  std::unordered_map<RoleKey, std::string, RoleKeyHash> new_roles;
  std::unordered_map<NamedRoleKey, std::vector<std::string>, NamedRoleKeyHash>
      new_named_roles;
  for (auto& record : records) {
    const auto vendor_id = record.vendor_id;
    if (new_records.contains(vendor_id)) {
      return std::unexpected("加载设备目录发现重复 vendor_id: " + vendor_id);
    }
    if (record.dcdw_label) {
      RoleKey key{record.school_id, *record.dcdw_label};
      if (new_roles.contains(key)) {
        return std::unexpected("加载设备目录发现同校角色号冲突: " +
                               *record.dcdw_label);
      }
      new_roles.emplace(std::move(key), vendor_id);
      new_named_roles[NamedRoleKey{record.school_name, *record.dcdw_label}]
          .push_back(vendor_id);
    }
    new_records.emplace(vendor_id, std::move(record));
  }
  records_.swap(new_records);
  roles_.swap(new_roles);
  named_roles_.swap(new_named_roles);
  return {};
}

std::expected<Mutation, std::string> DeviceRegistry::ApplyRegistration(
    const protocol::Registration& registration, TimePoint received_at) {
  const auto iterator = records_.find(registration.vendor_id);
  if (iterator == records_.end()) {
    return std::unexpected("未知设备: " + registration.vendor_id);
  }

  auto& record = iterator->second;
  if (record.DeviceType() != registration.device_type) {
    return std::unexpected("device_type 与已建档设备不一致");
  }
  DeviceRecord new_record = record;
  std::optional<std::string> diagnostic;
  if (registration.school_name &&
      *registration.school_name != record.school_name) {
    diagnostic = AppendDiagnostic(std::move(diagnostic), "拒绝设备自动迁移学校");
  }
  std::optional<RoleKey> old_role;
  std::optional<RoleKey> new_role;
  if (registration.dcdw_label &&
      registration.dcdw_label != record.dcdw_label) {
    const RoleKey candidate{record.school_id, *registration.dcdw_label};
    const auto owner = roles_.find(candidate);
    if (owner != roles_.end() && owner->second != record.vendor_id) {
      diagnostic = AppendDiagnostic(std::move(diagnostic),
                                    "拒绝同校冲突角色号");
    } else {
      if (record.dcdw_label) {
        old_role.emplace(RoleKey{record.school_id, *record.dcdw_label});
      }
      new_record.dcdw_label = registration.dcdw_label;
      if (owner == roles_.end()) new_role.emplace(candidate);
    }
  }

  const bool online =
      registration.status == protocol::RegistrationStatus::kOnline;
  new_record.status = online ? Status::kOnline : Status::kOffline;
  if (online) new_record.last_seen_at = received_at;
  ++new_record.revision;
  Mutation mutation{
      new_record,
      online ? state_event::ChangeReason::kRegistrationOnline
             : state_event::ChangeReason::kRegistrationOffline,
      record.status != new_record.status,
      std::move(diagnostic)};
  if (old_role) RemoveNamedRole(record);
  if (new_role) roles_.emplace(*new_role, record.vendor_id);
  using std::swap;
  swap(record, new_record);
  if (new_role) AddNamedRole(record);
  if (old_role) roles_.erase(*old_role);
  return mutation;
}

std::expected<Mutation, std::string> DeviceRegistry::ApplyTelemetry(
    std::string_view vendor_id, protocol::Telemetry telemetry,
    TimePoint received_at) {
  const auto iterator = records_.find(std::string{vendor_id});
  if (iterator == records_.end()) {
    return std::unexpected("未知设备: " + std::string{vendor_id});
  }

  auto& record = iterator->second;
  if (telemetry.device_type && *telemetry.device_type != record.DeviceType()) {
    return std::unexpected("device_type 与已建档设备不一致");
  }
  DeviceRecord new_record{
      .vendor_id = record.vendor_id,
      .school_id = record.school_id,
      .school_name = record.school_name,
      .dcdw_label = record.dcdw_label,
      .model_version = record.model_version,
      .status = Status::kOnline,
      .last_seen_at = received_at,
      .latest_telemetry = std::move(telemetry.payload),
      .telemetry_received_at = received_at,
      .revision = record.revision + 1,
      .device_type = record.device_type,
  };
  std::optional<std::string> diagnostic;
  std::optional<RoleKey> new_role;
  if (!record.dcdw_label && telemetry.dcdw_label) {
    const RoleKey candidate{record.school_id, *telemetry.dcdw_label};
    const auto owner = roles_.find(candidate);
    if (owner != roles_.end() && owner->second != record.vendor_id) {
      diagnostic = "拒绝同校冲突角色号";
    } else {
      new_record.dcdw_label = telemetry.dcdw_label;
      if (owner == roles_.end()) new_role.emplace(candidate);
    }
  }
  Mutation mutation{new_record, state_event::ChangeReason::kTelemetry,
                    record.status != new_record.status,
                    std::move(diagnostic)};
  if (new_role) roles_.emplace(*new_role, record.vendor_id);
  using std::swap;
  swap(record, new_record);
  if (new_role) AddNamedRole(record);
  return mutation;
}

std::vector<Mutation> DeviceRegistry::ExpireInactive(
    TimePoint now, std::chrono::seconds timeout) {
  std::vector<Mutation> mutations;
  std::vector<DeviceRecord*> expired;
  for (auto& [vendor_id, record] : records_) {
    static_cast<void>(vendor_id);
    if (record.status != Status::kOnline || !record.last_seen_at ||
        now - *record.last_seen_at < timeout) {
      continue;
    }
    expired.push_back(&record);
  }
  mutations.reserve(expired.size());
  std::vector<DeviceRecord> replacements;
  replacements.reserve(expired.size());
  for (const auto* record : expired) {
    auto replacement = *record;
    replacement.status = Status::kOffline;
    ++replacement.revision;
    mutations.push_back(
        Mutation{replacement, state_event::ChangeReason::kActivityTimeout,
                 true, std::nullopt});
    replacements.push_back(std::move(replacement));
  }
  using std::swap;
  for (std::size_t index = 0; index < expired.size(); ++index) {
    swap(*expired[index], replacements[index]);
  }
  return mutations;
}

std::expected<void, std::string> DeviceRegistry::AddProvisioned(
    DeviceRecord record) {
  if (records_.contains(record.vendor_id)) {
    return std::unexpected("重复 vendor_id: " + record.vendor_id);
  }
  std::optional<decltype(roles_)::iterator> inserted_role;
  if (record.dcdw_label) {
    RoleKey key{record.school_id, *record.dcdw_label};
    if (roles_.contains(key)) {
      return std::unexpected("同校角色号冲突: " + *record.dcdw_label);
    }
    inserted_role = roles_.emplace(std::move(key), record.vendor_id).first;
  }
  try {
    const auto [iterator, inserted] =
        records_.emplace(record.vendor_id, std::move(record));
    static_cast<void>(iterator);
    if (!inserted) {
      if (inserted_role) roles_.erase(*inserted_role);
      return std::unexpected("重复 vendor_id");
    }
    AddNamedRole(iterator->second);
  } catch (...) {
    if (inserted_role) roles_.erase(*inserted_role);
    throw;
  }
  return {};
}

std::vector<DeviceRecord> DeviceRegistry::ListOnlineDevices() const {
  std::vector<DeviceRecord> online;
  online.reserve(records_.size());
  for (const auto& [vendor_id, record] : records_) {
    static_cast<void>(vendor_id);
    if (record.status == Status::kOnline) online.push_back(record);
  }
  std::ranges::sort(online, {}, &DeviceRecord::vendor_id);
  return online;
}

std::vector<DeviceRecord> DeviceRegistry::ListDevices() const {
  std::vector<DeviceRecord> devices;
  devices.reserve(records_.size());
  for (const auto& [vendor_id, record] : records_) {
    static_cast<void>(vendor_id);
    devices.push_back(record);
  }
  std::ranges::sort(devices, {}, &DeviceRecord::vendor_id);
  return devices;
}

const DeviceRecord* DeviceRegistry::Find(std::string_view vendor_id) const {
  const auto iterator = records_.find(std::string{vendor_id});
  return iterator == records_.end() ? nullptr : &iterator->second;
}

const DeviceRecord* DeviceRegistry::FindBySchoolAndLabel(
    std::int64_t school_id, std::string_view label) const {
  const auto role = roles_.find(RoleKey{school_id, std::string{label}});
  return role == roles_.end() ? nullptr : Find(role->second);
}

const DeviceRecord* DeviceRegistry::FindBySchoolNameAndLabel(
    std::string_view school_name, std::string_view label) const {
  const auto role = named_roles_.find(
      NamedRoleKey{std::string{school_name}, std::string{label}});
  if (role == named_roles_.end() || role->second.size() != 1) return nullptr;
  return Find(role->second.front());
}

bool DeviceRegistry::IsSchoolNameAndLabelAmbiguous(
    std::string_view school_name, std::string_view label) const {
  const auto role = named_roles_.find(
      NamedRoleKey{std::string{school_name}, std::string{label}});
  return role != named_roles_.end() && role->second.size() > 1;
}

}  // namespace cns::device
