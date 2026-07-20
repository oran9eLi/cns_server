#pragma once

#include <chrono>
#include <string>

namespace cns::protocol {

std::string FormatUtcRfc3339Millis(
    std::chrono::system_clock::time_point value);

}  // namespace cns::protocol
