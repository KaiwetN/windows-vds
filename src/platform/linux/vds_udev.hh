// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>
#pragma once

#include <string>
#include <vector>

struct udev;
struct udev_monitor;

namespace vds {

std::vector<std::string> discover_vds_devices();
bool bluetooth_hid_device_present(const std::string &address);

class VdsDeviceMonitor {
public:
  VdsDeviceMonitor();
  ~VdsDeviceMonitor();

  VdsDeviceMonitor(const VdsDeviceMonitor &) = delete;
  VdsDeviceMonitor &operator=(const VdsDeviceMonitor &) = delete;
  VdsDeviceMonitor(VdsDeviceMonitor &&other) noexcept;
  VdsDeviceMonitor &operator=(VdsDeviceMonitor &&other) noexcept;

  int fd() const;
  bool drain();

private:
  udev *udev_ = nullptr;
  udev_monitor *monitor_ = nullptr;
};

} // namespace vds
