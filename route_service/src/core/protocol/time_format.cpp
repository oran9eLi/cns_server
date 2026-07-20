#include "core/protocol/time_format.hpp"

#include <array>
#include <cstdio>

namespace cns::protocol {

std::string FormatUtcRfc3339Millis(
    std::chrono::system_clock::time_point value) {
  const auto milliseconds = std::chrono::floor<std::chrono::milliseconds>(value);
  const auto days = std::chrono::floor<std::chrono::days>(milliseconds);
  const std::chrono::year_month_day date{days};
  const std::chrono::hh_mm_ss time{milliseconds - days};
  std::array<char, 25> output{};
  std::snprintf(output.data(), output.size(),
                "%04d-%02u-%02uT%02lld:%02lld:%02lld.%03lldZ",
                static_cast<int>(date.year()),
                static_cast<unsigned>(date.month()),
                static_cast<unsigned>(date.day()),
                static_cast<long long>(time.hours().count()),
                static_cast<long long>(time.minutes().count()),
                static_cast<long long>(time.seconds().count()),
                static_cast<long long>(time.subseconds().count()));
  return output.data();
}

}  // namespace cns::protocol
