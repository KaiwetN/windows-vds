// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace vds {

bool is_decimal_number(std::string_view text);
std::string hex_u8(std::uint8_t value);
std::string hex_u16(std::uint16_t value);
std::string hex_bytes(std::span<const std::uint8_t> bytes,
                      std::size_t max_bytes, char separator = ' ',
                      bool append_ellipsis = true);

template <class Rep, class Period>
std::uint64_t duration_us(std::chrono::duration<Rep, Period> duration) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(duration).count());
}

} // namespace vds
