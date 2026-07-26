// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include "vds_io.hh"

namespace vds {

void set_nonblocking(int fd, std::string_view label) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    throw std::runtime_error("failed to read " + std::string(label) +
                             " flags: " + std::strerror(errno));
  }
  if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    throw std::runtime_error("failed to set " + std::string(label) +
                             " nonblocking: " + std::strerror(errno));
  }
}

void write_full(int fd, std::span<const std::uint8_t> bytes,
                std::string_view label, int poll_timeout_ms) {
  const std::uint8_t *data = bytes.data();
  std::size_t size = bytes.size();
  while (size > 0) {
    const ssize_t written = ::write(fd, data, size);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      if ((errno == EAGAIN || errno == EWOULDBLOCK) && poll_timeout_ms >= 0) {
        pollfd pfd{.fd = fd, .events = POLLOUT, .revents = 0};
        const int ready = ::poll(&pfd, 1, poll_timeout_ms);
        if (ready > 0 && (pfd.revents & POLLOUT) != 0) {
          continue;
        }
        if (ready == 0) {
          throw std::runtime_error(std::string(label) + " write timed out");
        }
        if (ready < 0 && errno == EINTR) {
          continue;
        }
        if (ready > 0) {
          throw std::runtime_error(std::string(label) +
                                   " write poll reported socket error");
        }
        throw std::runtime_error(std::string(label) +
                                 " write poll failed: " + std::strerror(errno));
      }
      throw std::runtime_error(std::string(label) +
                               " write failed: " + std::strerror(errno));
    }
    if (written == 0) {
      throw std::runtime_error(std::string(label) +
                               " write returned zero bytes");
    }
    data += written;
    size -= static_cast<std::size_t>(written);
  }
}

void write_full(int fd, std::string_view text, std::string_view label) {
  const auto *data = reinterpret_cast<const std::uint8_t *>(text.data());
  write_full(fd, std::span<const std::uint8_t>(data, text.size()), label);
}

} // namespace vds
