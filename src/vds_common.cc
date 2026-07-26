// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>

#include "vds_common.hh"

#include <algorithm>
#include <cctype>

namespace vds {

namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

void append_byte_hex_digits(std::string &text, std::uint8_t value) {
  text.push_back(kHexDigits[(value >> 4) & 0x0f]);
  text.push_back(kHexDigits[value & 0x0f]);
}

} // namespace

bool is_decimal_number(std::string_view text) {
  return !text.empty() && std::all_of(text.begin(), text.end(), [](char ch) {
    return std::isdigit(static_cast<unsigned char>(ch)) != 0;
  });
}

std::string hex_u8(std::uint8_t value) {
  std::string text = "0x";
  append_byte_hex_digits(text, value);
  return text;
}

std::string hex_u16(std::uint16_t value) {
  std::string text = "0x0000";
  text[2] = kHexDigits[(value >> 12) & 0x0f];
  text[3] = kHexDigits[(value >> 8) & 0x0f];
  text[4] = kHexDigits[(value >> 4) & 0x0f];
  text[5] = kHexDigits[value & 0x0f];
  return text;
}

std::string hex_bytes(std::span<const std::uint8_t> bytes,
                      std::size_t max_bytes, char separator,
                      bool append_ellipsis) {
  const std::size_t count = std::min(bytes.size(), max_bytes);
  std::string text;
  text.reserve(count * 3 + (bytes.size() > max_bytes ? 4 : 0));

  for (std::size_t index = 0; index < count; ++index) {
    if (index > 0 && separator != '\0') {
      text.push_back(separator);
    }
    append_byte_hex_digits(text, bytes[index]);
  }
  if (append_ellipsis && bytes.size() > max_bytes) {
    text += " ...";
  }
  return text;
}

} // namespace vds
