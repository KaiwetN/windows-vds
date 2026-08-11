// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace vds {

// Feature report ids used to query physical DualSense / DualSense Edge
// information. The layout was verified against ds.evua.cc and the Linux
// hid-playstation driver:
//   - 0x20 (64 bytes): hw_version LE32 @24, fw_version LE32 @28,
//     update_version ("A-xxxx") LE16 @44, serial parts @1..20.
//   - 0x09 (20 bytes): pairing info, controller MAC address @1..6.
//   - 0x80/0x81: vendor sub-command pair. Sending [1, 19] on 0x80 returns
//     the 17-byte shell serial in 0x81 @4..21; [21, 5, side] returns the
//     DualSense Edge stick module status in 0x81 byte 5.
constexpr std::uint8_t kDsFeatureReportFirmwareInfo = 0x20;
constexpr std::uint8_t kDsFeatureReportPairingInfo = 0x09;
// DualShock 4 firmware info: build date @1..15, build time @17..24,
// hw major LE16 @33, hw minor LE16 @35, sw major LE32 @37.
constexpr std::uint8_t kDs4FeatureReportInfo = 0xa3;
// DS4 Bluetooth MAC address, 6 bytes at offset 1 (Linux hid-sony
// sony_get_usb_ds4_devaddr and DS4Windows read this report). The bytes
// are stored in reverse (little-endian) order, same as DualSense 0x09;
// hid-sony prints the canonical address with %pMR.
constexpr std::uint8_t kDs4FeatureReportPairingInfo = 0x12;
constexpr std::uint8_t kDsVendorSetReport = 0x80;
constexpr std::uint8_t kDsVendorGetReport = 0x81;

struct VdsControllerInfo {
  std::string model;            // DualSense / DualSense Edge
  std::string connection;       // usb / bluetooth
  std::string serial;           // serial printed on the controller shell
  std::string firmware;         // "A-0402" style update version
  std::string firmware_version; // raw main firmware version, e.g. "0x0630"
  std::string hardware_version; // raw hw version, e.g. "0x0400"
  std::string hardware_model;   // motherboard: BDM-xxx / HMB-010 / HDM-010
  std::string build_time;       // "2026年2月4日 12时34分56秒" style
  std::string color_code;       // serial digits 4..6
  std::string color_name;       // 星尘白 / 午夜黑 / ...
  std::string mac_address;      // controller MAC from pairing report
  std::string left_module;      // Edge stick modules
  std::string right_module;
  bool info_read = false;       // whether a firmware info report was read
  bool is_clone = false;        // heuristic clone detection
  std::string error;            // first read failure, for diagnostics
};

std::string vds_hardware_model_name(std::uint32_t hw_version, bool edge);
std::string vds_ds4_board_model(std::uint16_t hw_version_minor);
std::string vds_format_update_version(std::uint16_t update_version);
std::string vds_format_firmware_version(std::uint32_t firmware_version);
std::string vds_controller_color_code(std::string_view serial);
std::string vds_controller_color_name(std::string_view serial);
std::string vds_format_build_time(std::string_view date,
                                  std::string_view time);
std::string vds_module_status(std::uint8_t value);
std::string vds_controller_info_json(const VdsControllerInfo &info);
std::string vds_controller_info_object(const VdsControllerInfo &info);

// Parsers over raw feature report bytes (report id lives at [0]).
std::string vds_serial_from_info_report(std::span<const std::uint8_t> report);
std::string vds_build_time_from_info_report(std::span<const std::uint8_t> report);
std::string vds_serial_from_vendor_report(std::span<const std::uint8_t> report);
std::string vds_mac_from_pairing_report(std::span<const std::uint8_t> report);
std::uint32_t
vds_hw_version_from_info_report(std::span<const std::uint8_t> report);
std::uint32_t
vds_fw_version_from_info_report(std::span<const std::uint8_t> report);
std::uint16_t
vds_update_version_from_info_report(std::span<const std::uint8_t> report);
std::string vds_build_time_from_ds4_report(std::span<const std::uint8_t> report);
std::uint16_t
vds_ds4_hw_major_from_report(std::span<const std::uint8_t> report);
std::uint16_t
vds_ds4_hw_minor_from_report(std::span<const std::uint8_t> report);
std::uint32_t
vds_ds4_sw_major_from_report(std::span<const std::uint8_t> report);
std::uint8_t
vds_module_status_from_vendor_report(std::span<const std::uint8_t> report);

} // namespace vds
