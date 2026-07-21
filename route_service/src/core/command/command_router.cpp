#include "core/command/command_router.hpp"

#include <utility>

namespace cns::command {
namespace {

ProtocolError Error(std::string code, std::string message) {
  return {std::move(code), std::move(message)};
}

ResolvedTarget Resolve(const device::DeviceRecord& record) {
  return {record.vendor_id, record.school_name, record.dcdw_label};
}

}  // namespace

std::expected<void, std::string> SourceCatalog::Load(
    std::vector<CommandSource> sources) {
  std::unordered_map<std::string, CommandSource> loaded;
  loaded.reserve(sources.size());
  for (auto& source : sources) {
    if (source.source_id.empty()) {
      return std::unexpected("命令来源标识不能为空");
    }
    if (source.kind == SourceKind::kDevice) {
      if (!source.device_vendor_id) {
        return std::unexpected("设备来源缺少关联设备");
      }
      if (*source.device_vendor_id != source.source_id) {
        return std::unexpected("设备来源标识必须等于关联设备标识");
      }
    } else if (source.device_vendor_id) {
      return std::unexpected("非设备来源不得关联设备");
    }
    const auto source_id = source.source_id;
    if (!loaded.emplace(source_id, std::move(source)).second) {
      return std::unexpected("命令来源标识重复: " + source_id);
    }
  }
  sources_.swap(loaded);
  return {};
}

const CommandSource* SourceCatalog::Find(std::string_view source_id) const {
  const auto iterator = sources_.find(std::string{source_id});
  return iterator == sources_.end() ? nullptr : &iterator->second;
}

std::expected<ResolvedTarget, ProtocolError> ResolveConfigTarget(
    const CommandSource& source, const SourceConfigRequest& request,
    const device::DeviceRegistry& devices) {
  const device::DeviceRecord* target = nullptr;
  if (source.kind == SourceKind::kDevice) {
    const auto* label = std::get_if<DeviceLabelTarget>(&request.target);
    if (label == nullptr || !source.device_vendor_id) {
      return std::unexpected(
          Error("invalid_target", "设备来源只能使用同校角色号寻址"));
    }
    const auto* source_device = devices.Find(*source.device_vendor_id);
    if (source_device == nullptr) {
      return std::unexpected(
          Error("permission_denied", "来源设备不在设备目录中"));
    }
    if (source_device->status != device::Status::kOnline) {
      return std::unexpected(
          Error("source_device_offline", "来源设备当前离线"));
    }
    target = devices.FindBySchoolAndLabel(source_device->school_id,
                                          label->dcdw_label);
  } else if (const auto* vendor = std::get_if<VendorTarget>(&request.target)) {
    target = devices.Find(vendor->vendor_id);
  } else if (const auto* label =
                 std::get_if<SchoolLabelTarget>(&request.target)) {
    if (devices.IsSchoolNameAndLabelAmbiguous(label->school_name,
                                               label->dcdw_label)) {
      return std::unexpected(
          Error("target_ambiguous", "学校名和角色号对应多个设备"));
    }
    target = devices.FindBySchoolNameAndLabel(label->school_name,
                                               label->dcdw_label);
  } else {
    return std::unexpected(
        Error("invalid_target", "命令来源不能使用该目标形式"));
  }
  if (target == nullptr) {
    return std::unexpected(Error("target_not_found", "目标设备不存在"));
  }
  return Resolve(*target);
}

std::expected<ResolvedTarget, ProtocolError> ResolveControlTarget(
    const CommandSource& source, const SourceControlRequest& request,
    const device::DeviceRegistry& devices) {
  return ResolveConfigTarget(
      source,
      SourceConfigRequest{request.request_id, request.target, {},
                          request.comparison_payload},
      devices);
}

}  // namespace cns::command
