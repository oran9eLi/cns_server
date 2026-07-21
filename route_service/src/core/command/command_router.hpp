#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/command/command_types.hpp"
#include "core/device/device_registry.hpp"

namespace cns::command {

struct CommandSource {
  std::string source_id;
  SourceKind kind;
  std::optional<std::string> device_vendor_id;
  bool enabled;
};

class SourceCatalog {
 public:
  std::expected<void, std::string> Load(std::vector<CommandSource> sources);
  const CommandSource* Find(std::string_view source_id) const;

 private:
  std::unordered_map<std::string, CommandSource> sources_;
};

struct ResolvedTarget {
  std::string vendor_id;
  std::string school_name;
  std::optional<std::string> dcdw_label;
};

std::expected<ResolvedTarget, ProtocolError> ResolveConfigTarget(
    const CommandSource& source, const SourceConfigRequest& request,
    const device::DeviceRegistry& devices);
std::expected<ResolvedTarget, ProtocolError> ResolveControlTarget(
    const CommandSource& source, const SourceControlRequest& request,
    const device::DeviceRegistry& devices);

}  // namespace cns::command
