/**
 * @file config_policy.hpp
 * @brief 配置参数白名单、设备下发 JSON 与设备 ACK 解析。
 */
#pragma once

#include "core/command/command_types.hpp"

#include <expected>
#include <string_view>

namespace cns::command {

nlohmann::json BuildDeviceConfigSet(std::string_view command_id,
                                    const ConfigParameters& parameters);
std::expected<DeviceConfigAck, ProtocolError> ParseDeviceConfigAck(
    std::string_view payload);

}  // namespace cns::command
