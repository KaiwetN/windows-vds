// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "unique_fd.hh"

namespace vds {

struct BtAcceptedChannel {
  std::string address;
  UniqueFd fd;
};

class BtL2capAcceptor {
public:
  BtL2capAcceptor();

  BtL2capAcceptor(const BtL2capAcceptor &) = delete;
  BtL2capAcceptor &operator=(const BtL2capAcceptor &) = delete;
  BtL2capAcceptor(BtL2capAcceptor &&other) noexcept = default;
  BtL2capAcceptor &operator=(BtL2capAcceptor &&other) noexcept = default;

  int control_listener_fd() const { return control_listener_fd_.get(); }
  int interrupt_listener_fd() const { return interrupt_listener_fd_.get(); }
  std::optional<BtAcceptedChannel> accept_control();
  std::optional<BtAcceptedChannel> accept_interrupt();

private:
  UniqueFd control_listener_fd_;
  UniqueFd interrupt_listener_fd_;
};

class BtL2capBackend {
public:
  BtL2capBackend(std::string address, UniqueFd control_fd,
                 UniqueFd interrupt_fd);
  ~BtL2capBackend();

  BtL2capBackend(const BtL2capBackend &) = delete;
  BtL2capBackend &operator=(const BtL2capBackend &) = delete;
  BtL2capBackend(BtL2capBackend &&other) noexcept;
  BtL2capBackend &operator=(BtL2capBackend &&other) noexcept;

  int control_fd() const { return control_fd_; }
  int interrupt_fd() const { return interrupt_fd_; }
  void send_output_report(std::span<const std::uint8_t> report);
  bool try_send_output_report(std::span<const std::uint8_t> report);
  void send_feature_get(std::uint8_t report_id);
  void send_feature_set(std::span<const std::uint8_t> report);
  std::optional<std::vector<std::uint8_t>> read_feature_report();
  std::optional<std::vector<std::uint8_t>> read_interrupt_packet();

private:
  std::string address_;
  int control_fd_ = -1;
  int interrupt_fd_ = -1;
};

} // namespace vds
