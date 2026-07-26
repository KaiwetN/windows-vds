// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "uapi/vds.h"

namespace vds::win {

struct Frame {
  vds_frame_header header{};
  std::vector<std::uint8_t> payload;
};

struct HandleIoCancellation {
  HANDLE stop_event = nullptr;
  const std::atomic_bool *stop_requested = nullptr;
  bool operation_aborted_is_stop = false;
};

void read_handle_exact(HANDLE handle, void *buffer, std::size_t size,
                       std::string_view label,
                       const HandleIoCancellation &cancellation = {});
void write_handle_exact(HANDLE handle, std::span<const std::uint8_t> bytes,
                        std::string_view label,
                        const HandleIoCancellation &cancellation = {});
Frame read_handle_frame(HANDLE handle, std::string_view label,
                        const HandleIoCancellation &cancellation = {});
void write_handle_frame(HANDLE handle, std::uint16_t type,
                        std::span<const std::uint8_t> payload,
                        std::string_view label,
                        const HandleIoCancellation &cancellation = {});

} // namespace vds::win
