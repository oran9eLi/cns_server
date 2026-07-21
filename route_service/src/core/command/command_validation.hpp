#pragma once

#include <cctype>
#include <ranges>
#include <string_view>

namespace cns::command::validation {

inline bool IsUuidV4Shape(std::string_view value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-') {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) continue;
    const auto character = static_cast<unsigned char>(value[index]);
    if (std::isdigit(character) == 0 && (character < 'a' || character > 'f')) {
      return false;
    }
  }
  return value[14] == '4' && (value[19] == '8' || value[19] == '9' ||
                              value[19] == 'a' || value[19] == 'b');
}

inline bool IsValidErrorCode(std::string_view value) {
  return !value.empty() && value.size() <= 64 &&
         std::ranges::all_of(value, [](unsigned char character) {
           return (character >= 'a' && character <= 'z') ||
                  (character >= '0' && character <= '9') || character == '_';
         });
}

}  // namespace cns::command::validation
