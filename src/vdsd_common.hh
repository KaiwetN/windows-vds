// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "vds_config.hh"

namespace vds {

class Logger;

inline constexpr std::uint32_t kTraceInputAudio = 1u << 0;
inline constexpr std::uint32_t kTraceInputControl = 1u << 1;
inline constexpr std::uint32_t kTraceInput =
    kTraceInputAudio | kTraceInputControl;
inline constexpr std::uint32_t kTraceOutput = 1u << 2;
inline constexpr std::uint32_t kTraceAll = kTraceInput | kTraceOutput;

struct VdsdCommonOptions {
  std::string db_path;
  std::string log_path;
  bool help_requested = false;
};

struct VdsdControlControllerStatus {
  std::string address;
  bool connected = false;
  std::string path;
};

struct VdsdControlPortStatus {
  unsigned port = 0;
  std::string path;
  bool occupied = false;
  std::string address;
  std::string device_address;
};

struct VdsdControlPortCandidate {
  unsigned port = 0;
  std::string path;
};

struct VdsdControlPortBinding {
  unsigned port = 0;
  std::string address;
  std::string device_address;
};

enum class VdsdWorkerLaunchStatus {
  Ready,
  VirtualPortProviderUnavailable,
  DeviceAddressReserved,
  NoAvailablePort,
};

struct VdsdWorkerLaunchDecision {
  VdsdWorkerLaunchStatus status = VdsdWorkerLaunchStatus::NoAvailablePort;
  ControllerConfig config;
  unsigned port = 0;
  std::string device_address;
};

struct VdsdWorkerFailureBackoff {
  std::string address;
  std::chrono::steady_clock::time_point retry_after;
};

VdsdCommonOptions default_vdsd_common_options();
std::string vdsd_usage(std::string_view version, std::string_view build_year,
                       std::string_view platform_options);
void print_vdsd_usage(std::ostream &out, std::string_view version,
                      std::string_view build_year,
                      std::string_view platform_options);
bool parse_vdsd_common_option(int argc, char **argv, int &index,
                              VdsdCommonOptions &options);
std::string trim_command(std::string command);
std::uint32_t parse_trace_scope(std::string_view scope);
std::string trace_scope_name(std::uint32_t scope);
std::string active_trace_name(std::uint32_t trace_flags);
bool trace_enabled(std::uint32_t trace_flags, std::uint32_t target);
bool remember_vdsd_warning(std::vector<std::string> &warnings,
                           std::string_view key);
void forget_vdsd_warning(std::vector<std::string> &warnings,
                         std::string_view key);
bool remember_vdsd_warning_state(std::vector<std::string> &warnings,
                                 std::string_view key, std::string_view state);
void forget_vdsd_warning_state(std::vector<std::string> &warnings,
                               std::string_view key);
std::vector<VdsdControlPortStatus> build_vdsd_control_port_statuses(
    std::span<const VdsdControlPortCandidate> candidates,
    std::span<const VdsdControlPortBinding> bindings);
VdsdWorkerLaunchDecision select_vdsd_worker_launch_decision(
    const ControllerConfig &config, std::span<const unsigned> available_ports,
    std::span<const unsigned> reserved_ports, std::string_view device_address,
    bool virtual_port_provider_available,
    std::span<const std::string> reserved_device_addresses);
bool vdsd_worker_config_is_stale(const ConfigDb &db, std::string_view address,
                                 ControllerProfile profile, unsigned port,
                                 bool port_enabled);
void record_vdsd_worker_failure(
    std::vector<VdsdWorkerFailureBackoff> &backoffs, std::string_view address,
    std::chrono::steady_clock::time_point retry_after);
bool consume_vdsd_worker_retry(std::vector<VdsdWorkerFailureBackoff> &backoffs,
                               std::string_view address,
                               std::chrono::steady_clock::time_point now);
std::string handle_vdsd_control_command(
    std::string_view request, const std::string &db_path,
    std::span<const VdsdControlControllerStatus> controllers,
    std::span<const VdsdControlPortStatus> ports,
    const std::function<std::vector<ControllerTarget>()> &list_targets,
    std::uint32_t &trace_flags, bool &reload_requested, Logger &logger);

} // namespace vds
