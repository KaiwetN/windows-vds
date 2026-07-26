// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "vds_config.hh"

namespace vds {

std::optional<std::string> bluez_device_modalias(const std::string &address);
std::vector<ControllerTarget> list_bluez_controller_targets();
bool disconnect_bluez_device(const std::string &address);

} // namespace vds
