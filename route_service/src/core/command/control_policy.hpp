#pragma once

#include "core/command/command_types.hpp"

#include <expected>
#include <string_view>

namespace cns::command {

nlohmann::json BuildDeviceControlSet(std::string_view command_id,
                                     ControlCommand command,
                                     const ControlParameters& parameters);
std::expected<DeviceControlAck, ProtocolError> ParseDeviceControlAck(
    std::string_view payload);
std::uint16_t ExpectedMavlinkCommand(ControlCommand command) noexcept;

}  // namespace cns::command
