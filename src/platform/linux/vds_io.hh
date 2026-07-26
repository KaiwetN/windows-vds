// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace vds {

void set_nonblocking(int fd, std::string_view label);
void write_full(int fd, std::span<const std::uint8_t> bytes,
                std::string_view label, int poll_timeout_ms = -1);
void write_full(int fd, std::string_view text,
                std::string_view label = "write");

} // namespace vds
