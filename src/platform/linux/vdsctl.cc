// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>

#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "unique_fd.hh"
#include "vds_build_info.hh"
#include "vds_io.hh"
#include "vdsctl_common.hh"

namespace {

constexpr const char *kDefaultControlSocket = "/run/vdsd.sock";

std::string request_daemon(const std::string &socket_path,
                           const std::string &command) {
  vds::UniqueFd fd(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
  if (!fd) {
    throw std::runtime_error("failed to open daemon control socket: " +
                             std::string(std::strerror(errno)));
  }

  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (socket_path.size() >= sizeof(address.sun_path)) {
    throw std::runtime_error("control socket path is too long: " + socket_path);
  }
  std::strncpy(address.sun_path, socket_path.c_str(),
               sizeof(address.sun_path) - 1);

  if (::connect(fd.get(), reinterpret_cast<const sockaddr *>(&address),
                sizeof(address)) < 0) {
    throw std::runtime_error("failed to connect " + socket_path + ": " +
                             std::strerror(errno));
  }

  vds::write_full(fd.get(), command);
  (void)::shutdown(fd.get(), SHUT_WR);

  std::string response;
  std::array<char, 512> buffer{};
  while (true) {
    const ssize_t got = ::read(fd.get(), buffer.data(), buffer.size());
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw std::runtime_error("failed to read daemon response: " +
                               std::string(std::strerror(errno)));
    }
    if (got == 0) {
      break;
    }
    response.append(buffer.data(), static_cast<std::size_t>(got));
  }
  return response;
}

} // namespace

int main(int argc, char **argv) {
  return vds::run_vdsctl_app(argc, argv, vds::kVersion, vds::kBuildYear,
                             vds::VdsctlPlatform{
                                 .request_control =
                                     [](const std::string &command) {
                                       return request_daemon(
                                           kDefaultControlSocket, command);
                                     },
                             });
}
