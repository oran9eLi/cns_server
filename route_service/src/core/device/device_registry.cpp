#include "core/device/device_registry.hpp"

#include <functional>
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

std::size_t DeviceRegistry::RoleKeyHash::operator()(const RoleKey& key) const {
  const auto school_hash = std::hash<std::int64_t>{}(key.school_id);
  const auto label_hash = std::hash<std::string>{}(key.dcdw_label);
  return school_hash ^ (label_hash + 0x9e3779b9U + (school_hash << 6U) +
                        (school_hash >> 2U));
}

std::expected<void, std::string> DeviceRegistry::Load(
    std::vector<DeviceRecord> records) {
  std::unordered_map<std::string, DeviceRecord> new_records;
  std::unordered_map<RoleKey, std::string, RoleKeyHash> new_roles;
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
    }
    new_records.emplace(vendor_id, std::move(record));
  }
  records_.swap(new_records);
  roles_.swap(new_roles);
  return {};
}

std::expected<Mutation, std::string> DeviceRegistry::ApplyRegistration(
    const protocol::Registration& registration, TimePoint received_at) {
  if (!records_.contains(registration.vendor_id)) {
    return std::unexpected("未知设备: " + registration.vendor_id);
  }

  auto new_records = records_;
  auto new_roles = roles_;
  auto& record = new_records.at(registration.vendor_id);
  std::optional<std::string> diagnostic;
  if (registration.school_name &&
      *registration.school_name != record.school_name) {
    diagnostic = AppendDiagnostic(std::move(diagnostic), "拒绝设备自动迁移学校");
  }
  if (registration.dcdw_label &&
      registration.dcdw_label != record.dcdw_label) {
    const RoleKey candidate{record.school_id, *registration.dcdw_label};
    const auto owner = new_roles.find(candidate);
    if (owner != new_roles.end() && owner->second != record.vendor_id) {
      diagnostic = AppendDiagnostic(std::move(diagnostic),
                                    "拒绝同校冲突角色号");
    } else {
      if (record.dcdw_label) {
        new_roles.erase(RoleKey{record.school_id, *record.dcdw_label});
      }
      record.dcdw_label = registration.dcdw_label;
      new_roles.emplace(candidate, record.vendor_id);
    }
  }

  const bool online =
      registration.status == protocol::RegistrationStatus::kOnline;
  record.status = online ? Status::kOnline : Status::kOffline;
  record.last_seen_at = received_at;
  ++record.revision;
  Mutation mutation{
      record,
      online ? state_event::ChangeReason::kRegistrationOnline
             : state_event::ChangeReason::kRegistrationOffline,
      std::move(diagnostic)};
  records_.swap(new_records);
  roles_.swap(new_roles);
  return mutation;
}

std::expected<Mutation, std::string> DeviceRegistry::ApplyTelemetry(
    std::string_view vendor_id, protocol::Telemetry telemetry,
    TimePoint received_at) {
  const std::string vendor{vendor_id};
  if (!records_.contains(vendor)) {
    return std::unexpected("未知设备: " + std::string{vendor_id});
  }

  auto new_records = records_;
  auto new_roles = roles_;
  auto& record = new_records.at(vendor);
  std::optional<std::string> diagnostic;
  if (!record.dcdw_label && telemetry.dcdw_label) {
    const RoleKey candidate{record.school_id, *telemetry.dcdw_label};
    const auto owner = new_roles.find(candidate);
    if (owner != new_roles.end() && owner->second != record.vendor_id) {
      diagnostic = "拒绝同校冲突角色号";
    } else {
      record.dcdw_label = telemetry.dcdw_label;
      new_roles.emplace(candidate, record.vendor_id);
    }
  }
  record.latest_telemetry = std::move(telemetry.payload);
  record.telemetry_received_at = received_at;
  record.last_seen_at = received_at;
  record.status = Status::kOnline;
  ++record.revision;
  Mutation mutation{record, state_event::ChangeReason::kTelemetry,
                    std::move(diagnostic)};
  records_.swap(new_records);
  roles_.swap(new_roles);
  return mutation;
}

std::vector<Mutation> DeviceRegistry::ExpireInactive(
    TimePoint now, std::chrono::seconds timeout) {
  std::vector<Mutation> mutations;
  auto new_records = records_;
  for (auto& [vendor_id, record] : new_records) {
    static_cast<void>(vendor_id);
    if (record.status != Status::kOnline || !record.last_seen_at ||
        now - *record.last_seen_at < timeout) {
      continue;
    }
    record.status = Status::kOffline;
    ++record.revision;
    mutations.push_back(
        Mutation{record, state_event::ChangeReason::kActivityTimeout,
                 std::nullopt});
  }
  if (!mutations.empty()) records_.swap(new_records);
  return mutations;
}

std::expected<void, std::string> DeviceRegistry::AddProvisioned(
    DeviceRecord record) {
  auto new_records = records_;
  auto new_roles = roles_;
  if (new_records.contains(record.vendor_id)) {
    return std::unexpected("重复 vendor_id: " + record.vendor_id);
  }
  if (record.dcdw_label) {
    RoleKey key{record.school_id, *record.dcdw_label};
    if (new_roles.contains(key)) {
      return std::unexpected("同校角色号冲突: " + *record.dcdw_label);
    }
    new_roles.emplace(std::move(key), record.vendor_id);
  }
  new_records.emplace(record.vendor_id, std::move(record));
  records_.swap(new_records);
  roles_.swap(new_roles);
  return {};
}

const DeviceRecord* DeviceRegistry::Find(std::string_view vendor_id) const {
  const auto iterator = records_.find(std::string{vendor_id});
  return iterator == records_.end() ? nullptr : &iterator->second;
}

}  // namespace cns::device
