// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>

#include "vds_io.hh"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

#include "uapi/vds.h"
#include "unique_handle.hh"
#include "vds_protocol.hh"
#include "vds_win32.hh"

namespace {

bool is_stop_requested(const vds::win::HandleIoCancellation &cancellation) {
  return cancellation.operation_aborted_is_stop ||
         (cancellation.stop_requested != nullptr &&
          cancellation.stop_requested->load());
}

std::string stopped_message(std::string_view label,
                            std::string_view operation) {
  return std::string(label) + " " + std::string(operation) + " stopped";
}

std::string failed_message(std::string_view label, std::string_view operation,
                           DWORD error) {
  return std::string(label) + " " + std::string(operation) +
         " failed: " + vds::win::win32_error_message(error);
}

DWORD finish_overlapped_io(HANDLE handle, OVERLAPPED &overlapped,
                           std::string_view label, std::string_view operation,
                           const vds::win::HandleIoCancellation &cancellation) {
  DWORD transferred = 0;
  if (GetOverlappedResult(handle, &overlapped, &transferred, FALSE)) {
    return transferred;
  }

  DWORD error = GetLastError();
  if (error != ERROR_IO_INCOMPLETE) {
    if (error == ERROR_OPERATION_ABORTED && is_stop_requested(cancellation)) {
      throw std::runtime_error(stopped_message(label, operation));
    }
    throw std::runtime_error(failed_message(label, operation, error));
  }

  HANDLE wait_handles[2] = {overlapped.hEvent, cancellation.stop_event};
  const DWORD wait_count = cancellation.stop_event == nullptr ? 1 : 2;
  const DWORD wait_result =
      WaitForMultipleObjects(wait_count, wait_handles, FALSE, INFINITE);
  if (wait_result == WAIT_OBJECT_0 + 1) {
    CancelIoEx(handle, &overlapped);
    (void)GetOverlappedResult(handle, &overlapped, &transferred, TRUE);
    throw std::runtime_error(stopped_message(label, operation));
  }
  if (wait_result != WAIT_OBJECT_0) {
    throw std::runtime_error(
        std::string(label) + " " + std::string(operation) +
        " wait failed: " + vds::win::win32_error_message(GetLastError()));
  }

  if (!GetOverlappedResult(handle, &overlapped, &transferred, FALSE)) {
    error = GetLastError();
    if (error == ERROR_OPERATION_ABORTED && is_stop_requested(cancellation)) {
      throw std::runtime_error(stopped_message(label, operation));
    }
    throw std::runtime_error(failed_message(label, operation, error));
  }
  return transferred;
}

DWORD read_overlapped(HANDLE handle, void *buffer, DWORD size,
                      std::string_view label,
                      const vds::win::HandleIoCancellation &cancellation) {
  vds::win::UniqueHandle event(CreateEventA(nullptr, TRUE, FALSE, nullptr));
  if (!event) {
    throw std::runtime_error(std::string(label) + " read event failed: " +
                             vds::win::win32_error_message(GetLastError()));
  }

  OVERLAPPED overlapped{};
  overlapped.hEvent = event.get();
  if (!ReadFile(handle, buffer, size, nullptr, &overlapped)) {
    const DWORD error = GetLastError();
    if (error != ERROR_IO_PENDING) {
      throw std::runtime_error(std::string(label) + " read failed: " +
                               vds::win::win32_error_message(error));
    }
  }
  return finish_overlapped_io(handle, overlapped, label, "read", cancellation);
}

DWORD write_overlapped(HANDLE handle, const void *buffer, DWORD size,
                       std::string_view label,
                       const vds::win::HandleIoCancellation &cancellation) {
  vds::win::UniqueHandle event(CreateEventA(nullptr, TRUE, FALSE, nullptr));
  if (!event) {
    throw std::runtime_error(std::string(label) + " write event failed: " +
                             vds::win::win32_error_message(GetLastError()));
  }

  OVERLAPPED overlapped{};
  overlapped.hEvent = event.get();
  if (!WriteFile(handle, buffer, size, nullptr, &overlapped)) {
    const DWORD error = GetLastError();
    if (error != ERROR_IO_PENDING) {
      throw std::runtime_error(std::string(label) + " write failed: " +
                               vds::win::win32_error_message(error));
    }
  }
  return finish_overlapped_io(handle, overlapped, label, "write", cancellation);
}

} // namespace

namespace vds::win {

void read_handle_exact(HANDLE handle, void *buffer, std::size_t size,
                       std::string_view label,
                       const HandleIoCancellation &cancellation) {
  auto *data = static_cast<std::uint8_t *>(buffer);
  std::size_t remaining = size;
  while (remaining > 0) {
    const DWORD chunk =
        static_cast<DWORD>(std::min<std::size_t>(remaining, MAXDWORD));
    const DWORD got = read_overlapped(handle, data, chunk, label, cancellation);
    if (got == 0) {
      throw std::runtime_error(std::string(label) + " closed");
    }
    data += got;
    remaining -= got;
  }
}

void write_handle_exact(HANDLE handle, std::span<const std::uint8_t> bytes,
                        std::string_view label,
                        const HandleIoCancellation &cancellation) {
  const auto *data = bytes.data();
  std::size_t remaining = bytes.size();
  while (remaining > 0) {
    const DWORD chunk =
        static_cast<DWORD>(std::min<std::size_t>(remaining, 64u * 1024u));
    const DWORD written =
        write_overlapped(handle, data, chunk, label, cancellation);
    if (written == 0) {
      throw std::runtime_error(std::string(label) +
                               " write returned zero bytes");
    }
    data += written;
    remaining -= written;
  }
}

Frame read_handle_frame(HANDLE handle, std::string_view label,
                        const HandleIoCancellation &cancellation) {
  Frame frame;
  read_handle_exact(handle, &frame.header, sizeof(frame.header), label,
                    cancellation);
  if (frame.header.length > VDS_FRAME_MAX_PAYLOAD) {
    throw std::runtime_error(std::string(label) + " oversized frame length=" +
                             std::to_string(frame.header.length));
  }
  frame.payload.resize(frame.header.length);
  if (!frame.payload.empty()) {
    read_handle_exact(handle, frame.payload.data(), frame.payload.size(), label,
                      cancellation);
  }
  return frame;
}

void write_handle_frame(HANDLE handle, std::uint16_t type,
                        std::span<const std::uint8_t> payload,
                        std::string_view label,
                        const HandleIoCancellation &cancellation) {
  const std::vector<std::uint8_t> bytes = vds::frame_bytes(type, payload);
  write_handle_exact(handle, bytes, label, cancellation);
}

} // namespace vds::win
