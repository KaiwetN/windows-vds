// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "vds_config.hh"

namespace vds {

enum class VdsctlCommand {
  Attach,
  Detach,
  List,
  ListTargets,
  Trace,
  AudioBuffer,
  Effects,
};

struct VdsctlTraceCommand {
  bool enabled = false;
  std::string scope;
};

struct VdsctlAudioBufferCommand {
  std::optional<unsigned> chunks;
};

struct VdsctlPlatform {
  std::function<std::string(const std::string &)> request_control;
};

std::string vdsctl_usage(std::string_view version, std::string_view build_year);
int run_vdsctl_app(int argc, char **argv, std::string_view version,
                   std::string_view build_year, const VdsctlPlatform &platform);
VdsctlCommand parse_vdsctl_command(std::string_view command);
void require_vdsctl_arg_count(int argc, int expected);
ControllerConfig parse_vdsctl_attach(int argc, char **argv);
VdsctlTraceCommand parse_vdsctl_trace(int argc, char **argv);
VdsctlAudioBufferCommand parse_vdsctl_audio_buffer(int argc, char **argv);
std::string run_vdsctl_attach(
    int argc, char **argv,
    const std::function<std::string(const std::string &)> &request_control);
std::string run_vdsctl_detach(
    int argc, char **argv,
    const std::function<std::string(const std::string &)> &request_control);
std::string run_vdsctl_list(
    int argc,
    const std::function<std::string(const std::string &)> &request_control);
std::string run_vdsctl_list_targets(
    int argc,
    const std::function<std::string(const std::string &)> &request_control);
std::string run_vdsctl_trace(
    int argc, char **argv,
    const std::function<std::string(const std::string &)> &request_control);
std::string run_vdsctl_audio_buffer(
    int argc, char **argv,
    const std::function<std::string(const std::string &)> &request_control);
std::string run_vdsctl_effects(
    int argc, char **argv,
    const std::function<std::string(const std::string &)> &request_control);

} // namespace vds
