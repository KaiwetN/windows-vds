// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>
#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "uapi/vds.h"
#include "vds/ds5_protocol.h"
#include "vds_config.hh"

namespace vds {

struct DeviceId {
  std::uint16_t vendor;
  std::uint16_t product;
};

inline std::uint32_t
usb_profile_from_controller_profile(ControllerProfile profile) {
  switch (profile) {
  case ControllerProfile::Ds5:
    return VDS_PROFILE_DS5;
  case ControllerProfile::Dse:
    return VDS_PROFILE_DSE;
  case ControllerProfile::Unspecified:
    break;
  }
  throw std::runtime_error("empty profile has no fixed USB profile");
}

inline std::string usb_profile_name(std::uint32_t profile) {
  return profile == VDS_PROFILE_DSE ? "dse" : "ds5";
}

inline std::optional<unsigned int> parse_hex_token(std::string_view text,
                                                   std::size_t offset,
                                                   std::size_t max_digits) {
  unsigned int value = 0;
  std::size_t digits = 0;
  for (std::size_t i = offset; i < text.size() && digits < max_digits; ++i) {
    const char ch = text[i];
    unsigned int nibble;
    if (ch >= '0' && ch <= '9') {
      nibble = static_cast<unsigned int>(ch - '0');
    } else if (ch >= 'a' && ch <= 'f') {
      nibble = static_cast<unsigned int>(10 + ch - 'a');
    } else if (ch >= 'A' && ch <= 'F') {
      nibble = static_cast<unsigned int>(10 + ch - 'A');
    } else {
      break;
    }
    value = (value << 4) | nibble;
    ++digits;
  }

  if (digits == 0) {
    return std::nullopt;
  }
  return value;
}

inline std::optional<DeviceId> parse_modalias_device_id(std::string_view line) {
  const std::size_t vendor_marker = line.find('v');
  if (vendor_marker == std::string_view::npos) {
    return std::nullopt;
  }
  const std::size_t product_marker = line.find('p', vendor_marker + 1);
  if (product_marker == std::string_view::npos) {
    return std::nullopt;
  }

  const bool usb_modalias = line.find("usb:") != std::string_view::npos;
  const std::size_t digits = usb_modalias ? 4 : 8;
  const auto vendor = parse_hex_token(line, vendor_marker + 1, digits);
  const auto product = parse_hex_token(line, product_marker + 1, digits);
  if (!vendor || !product) {
    return std::nullopt;
  }

  return DeviceId{.vendor = static_cast<std::uint16_t>(*vendor & 0xffffu),
                  .product = static_cast<std::uint16_t>(*product & 0xffffu)};
}

inline std::optional<ControllerProfile>
controller_profile_from_device_id(const DeviceId &id) {
  if (id.vendor != VDS_SONY_VENDOR_ID) {
    return std::nullopt;
  }
  if (id.product == VDS_DS5_PRODUCT_ID) {
    return ControllerProfile::Ds5;
  }
  if (id.product == VDS_DSE_PRODUCT_ID) {
    return ControllerProfile::Dse;
  }
  return std::nullopt;
}

inline std::optional<ControllerProfile>
controller_profile_from_modalias(std::string_view modalias) {
  const auto id = parse_modalias_device_id(modalias);
  if (!id) {
    return std::nullopt;
  }
  return controller_profile_from_device_id(*id);
}

} // namespace vds
