/**
 * @file source_request.hpp
 * @brief 解析来源配置请求 topic、设备配置 ACK topic 与 JSON 信封。
 */
#pragma once

#include "core/command/command_types.hpp"

#include <expected>
#include <string>
#include <string_view>

namespace cns::command {

std::expected<std::string, ProtocolError> ParseSourceRequestTopic(
    std::string_view topic_namespace, std::string_view topic);
std::expected<std::string, ProtocolError> ParseDeviceConfigAckTopic(
    std::string_view topic_namespace, std::string_view topic);
std::expected<std::string, ProtocolError> ParseSourceControlRequestTopic(
    std::string_view topic_namespace, std::string_view topic);
std::expected<std::string, ProtocolError> ParseDeviceControlAckTopic(
    std::string_view topic_namespace, std::string_view topic);
SourceRequestParseResult ParseSourceConfigRequest(std::string_view payload,
                                                  SourceKind kind);
ParsedSourceRequest ParseSourceControlRequest(std::string_view payload,
                                              SourceKind kind);

}  // namespace cns::command
