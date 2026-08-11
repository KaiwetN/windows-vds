// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>

#include "vds_device_info.hh"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <string>
#include <utility>

#include "jsonl.hh"

namespace vds {

namespace {

constexpr char kHexDigits[] = "0123456789ABCDEF";

std::string uppercase(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
    return static_cast<char>(std::toupper(ch));
  });
  return text;
}

std::string trim_ascii(std::string_view text) {
  std::string result;
  result.reserve(text.size());
  for (const char ch : text) {
    if (ch == '\0') {
      break;
    }
    result.push_back(ch);
  }
  while (!result.empty() && std::isspace(
                                static_cast<unsigned char>(result.back())) != 0) {
    result.pop_back();
  }
  return result;
}

bool parse_unsigned(std::string_view text, unsigned &value) {
  if (text.empty()) {
    return false;
  }
  unsigned result = 0;
  for (const char ch : text) {
    if (std::isdigit(static_cast<unsigned char>(ch)) == 0) {
      return false;
    }
    result = result * 10 + static_cast<unsigned>(ch - '0');
  }
  value = result;
  return true;
}

std::string format_u16_hex(std::uint16_t value) {
  std::string text = "0000";
  text[0] = kHexDigits[(value >> 12) & 0x0f];
  text[1] = kHexDigits[(value >> 8) & 0x0f];
  text[2] = kHexDigits[(value >> 4) & 0x0f];
  text[3] = kHexDigits[value & 0x0f];
  return text;
}

std::string format_u32_hex(std::uint32_t value) {
  std::string text = "00000000";
  for (std::size_t index = 0; index < 8; ++index) {
    text[index] = kHexDigits[(value >> (28 - static_cast<unsigned>(index) * 4)) &
                             0x0f];
  }
  return text;
}

std::uint32_t read_u32_le(std::span<const std::uint8_t> report,
                          std::size_t offset) {
  if (offset + 4 > report.size()) {
    return 0;
  }
  return static_cast<std::uint32_t>(report[offset]) |
         (static_cast<std::uint32_t>(report[offset + 1]) << 8) |
         (static_cast<std::uint32_t>(report[offset + 2]) << 16) |
         (static_cast<std::uint32_t>(report[offset + 3]) << 24);
}

std::uint16_t read_u16_le(std::span<const std::uint8_t> report,
                          std::size_t offset) {
  if (offset + 2 > report.size()) {
    return 0;
  }
  return static_cast<std::uint16_t>(
      report[offset] | (static_cast<std::uint16_t>(report[offset + 1]) << 8));
}

} // namespace

std::string vds_hardware_model_name(std::uint32_t hw_version, bool edge) {
  const unsigned board = (hw_version >> 8) & 0xff;
  if (edge) {
    return board == 2 ? "HDM-010" : "";
  }
  switch (board) {
  case 2:
    return "HMB-010";
  case 3:
    return "BDM-010";
  case 4:
    return "BDM-020";
  case 5:
    return "BDM-030";
  case 6:
    return "BDM-040";
  case 7:
  case 8:
    return "BDM-050";
  case 17:
    return "BDM-060M";
  case 19:
    return "BDM-060X";
  default:
    return "";
  }
}

std::string vds_ds4_board_model(std::uint16_t hw_version_minor) {
  // Mapping from dualshock-tools ds4-controller.js hwToBoardModel, keyed on
  // the high byte of the DS4 hw minor version read from the 0xA3 report.
  const unsigned board = hw_version_minor >> 8;
  if (board == 0x31) {
    return "JDM-001";
  }
  if (board == 0x43) {
    return "JDM-011";
  }
  if (board == 0x54) {
    return "JDM-030";
  }
  if (board >= 0x64 && board <= 0x74) {
    return "JDM-040";
  }
  if ((board > 0x80 && board < 0x84) || board == 0x93) {
    return "JDM-020";
  }
  if (board == 0x90 || board == 0xa0 || board == 0xa4) {
    return "JDM-050";
  }
  if (board == 0xb0) {
    return "JDM-055 (Scuf?)";
  }
  if (board == 0xb4) {
    return "JDM-055";
  }
  return {};
}

std::string vds_format_update_version(std::uint16_t update_version) {
  return "A-" + format_u16_hex(update_version);
}

std::string vds_format_firmware_version(std::uint32_t firmware_version) {
  return "0x" + format_u32_hex(firmware_version);
}

std::string vds_controller_color_code(std::string_view serial) {
  if (serial.size() < 6) {
    return {};
  }
  return uppercase(std::string(serial.substr(4, 2)));
}

std::string vds_controller_color_name(std::string_view serial) {
  const std::string code = vds_controller_color_code(serial);
  if (code.empty()) {
    return {};
  }
  static constexpr std::pair<const char *, const char *> kColors[] = {
      {"00", "星尘白"}, {"01", "午夜黑"}, {"02", "宇宙红"},
      {"03", "星幻粉"}, {"04", "银河紫"}, {"05", "星光蓝"},
      {"06", "灰色迷彩"}, {"07", "火山红"}, {"08", "纯银"},
      {"09", "钴蓝"}, {"10", "晶彩青"}, {"11", "晶彩靛"},
      {"12", "晶彩珍珠"}, {"30", "30周年纪念"}, {"Z1", "战神：诸神黄昏"},
      {"Z2", "漫威蜘蛛侠2"}, {"Z3", "宇宙机器人"}, {"Z4", "堡垒之夜"},
      {"Z5", "怪物猎人：荒野"}, {"Z6", "最后生还者"},
      {"Z7", "羊蹄山幽灵（金）"}, {"Z8", "羊蹄山幽灵（黑）"},
      {"ZA", "战神20周年"}, {"ZB", "冰晶蓝"}, {"ZC", "宇宙机器人欢悦"},
  };
  for (const auto &[key, name] : kColors) {
    if (code == key) {
      return name;
    }
  }
  return {};
}

std::string vds_format_build_time(std::string_view date,
                                  std::string_view time) {
  const auto parse_component = [](std::string_view text, std::size_t offset,
                                  std::size_t length,
                                  unsigned &value) -> bool {
    if (offset + length > text.size()) {
      return false;
    }
    return parse_unsigned(text.substr(offset, length), value);
  };

  unsigned year = 0;
  unsigned month = 0;
  unsigned day = 0;
  unsigned hour = 0;
  unsigned minute = 0;
  unsigned second = 0;
  const bool date_ok = date.size() >= 10 &&
                       parse_component(date, 0, 4, year) &&
                       parse_component(date, 5, 2, month) &&
                       parse_component(date, 8, 2, day);
  const bool time_ok = time.size() >= 8 &&
                       parse_component(time, 0, 2, hour) &&
                       parse_component(time, 3, 2, minute) &&
                       parse_component(time, 6, 2, second);
  static constexpr std::string_view kMonths[] = {
      "Jan", "Feb", "Mar", "Apr", "May", "Jun",
      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
  };
  unsigned month_name = 0;
  unsigned month_day = 0;
  unsigned month_year = 0;
  bool month_date_ok = false;
  if (!date_ok && date.size() >= 10) {
    const std::string_view month_text = date.substr(0, 3);
    unsigned month_index = 0;
    for (; month_index < 12; ++month_index) {
      if (month_text == kMonths[month_index]) {
        break;
      }
    }
    if (month_index < 12) {
      month_name = month_index + 1;
      std::size_t cursor = 3;
      while (cursor < date.size() &&
             std::isspace(static_cast<unsigned char>(date[cursor])) != 0) {
        ++cursor;
      }
      const std::size_t day_start = cursor;
      while (cursor < date.size() &&
             std::isdigit(static_cast<unsigned char>(date[cursor])) != 0) {
        ++cursor;
      }
      const bool day_ok =
          cursor > day_start && cursor - day_start <= 2 &&
          parse_unsigned(date.substr(day_start, cursor - day_start),
                         month_day);
      while (cursor < date.size() &&
             std::isspace(static_cast<unsigned char>(date[cursor])) != 0) {
        ++cursor;
      }
      const std::size_t year_start = cursor;
      while (cursor < date.size() &&
             std::isdigit(static_cast<unsigned char>(date[cursor])) != 0) {
        ++cursor;
      }
      const bool year_ok =
          cursor - year_start == 4 &&
          parse_unsigned(date.substr(year_start, 4), month_year);
      month_date_ok = day_ok && year_ok && month_day >= 1 && month_day <= 31;
      if (!month_date_ok) {
        month_name = 0;
      }
    }
  }
  if (!date_ok && !month_date_ok && !time_ok) {
    const std::string combined =
        trim_ascii(date) + (date.empty() ? "" : " ") + trim_ascii(time);
    return combined;
  }

  std::string result;
  if (date_ok) {
    result += std::to_string(year);
    result += "/";
    result += std::to_string(month);
    result += "/";
    result += std::to_string(day);
  } else if (month_date_ok) {
    result += std::to_string(month_year);
    result += "/";
    result += std::to_string(month_name);
    result += "/";
    result += std::to_string(month_day);
  }
  if (time_ok) {
    if (!result.empty()) {
      result += " ";
    }
    result += std::to_string(hour);
    result += ":";
    result += std::to_string(minute);
    result += ":";
    result += std::to_string(second);
  }
  return result;
}

std::string vds_module_status(std::uint8_t value) {
  if (value == 0x84) {
    return "已解锁";
  }
  if (value == 0x8c) {
    return "未解锁";
  }
  return "未知状态";
}

std::string vds_serial_from_info_report(std::span<const std::uint8_t> report) {
  if (report.size() < 21) {
    return {};
  }
  const std::string first =
      trim_ascii(std::string_view(
          reinterpret_cast<const char *>(report.data() + 1), 11));
  const std::string second =
      trim_ascii(std::string_view(
          reinterpret_cast<const char *>(report.data() + 12), 8));
  if (first.empty() && second.empty()) {
    return {};
  }
  if (first.empty()) {
    return second;
  }
  if (second.empty()) {
    return first;
  }
  return first + " " + second;
}

std::string
vds_build_time_from_info_report(std::span<const std::uint8_t> report) {
  if (report.size() < 21) {
    return {};
  }
  const std::string first =
      trim_ascii(std::string_view(
          reinterpret_cast<const char *>(report.data() + 1), 11));
  const std::string second =
      trim_ascii(std::string_view(
          reinterpret_cast<const char *>(report.data() + 12), 8));
  return vds_format_build_time(first, second);
}

std::string
vds_serial_from_vendor_report(std::span<const std::uint8_t> report) {
  if (report.size() < 21 || report[0] != kDsVendorGetReport ||
      report[1] != 0x01 || report[2] != 0x13 || report[3] != 0x02) {
    return {};
  }
  return trim_ascii(std::string_view(
      reinterpret_cast<const char *>(report.data() + 4), 17));
}

std::string vds_mac_from_pairing_report(std::span<const std::uint8_t> report) {
  if (report.size() < 7) {
    return {};
  }
  std::string mac;
  // The report carries the Bluetooth MAC with the bytes reversed, so emit
  // the canonical order (offset 6 down to 1).
  for (std::size_t index = 6; index >= 1; --index) {
    if (!mac.empty()) {
      mac.push_back(':');
    }
    mac.push_back(kHexDigits[(report[index] >> 4) & 0x0f]);
    mac.push_back(kHexDigits[report[index] & 0x0f]);
  }
  return mac;
}

std::uint32_t vds_hw_version_from_info_report(
    std::span<const std::uint8_t> report) {
  return read_u32_le(report, 24);
}

std::uint32_t
vds_fw_version_from_info_report(std::span<const std::uint8_t> report) {
  return read_u32_le(report, 28);
}

std::uint16_t vds_update_version_from_info_report(
    std::span<const std::uint8_t> report) {
  if (report.size() < 46) {
    return 0;
  }
  return static_cast<std::uint16_t>(
      report[44] | (static_cast<std::uint16_t>(report[45]) << 8));
}

std::string vds_build_time_from_ds4_report(
    std::span<const std::uint8_t> report) {
  if (report.size() < 32 || report[0] != kDs4FeatureReportInfo) {
    return {};
  }
  const std::string date =
      trim_ascii(std::string_view(reinterpret_cast<const char *>(report.data() + 1),
                                  15));
  const std::string time =
      trim_ascii(std::string_view(reinterpret_cast<const char *>(report.data() + 17),
                                  8));
  return vds_format_build_time(date, time);
}

std::uint16_t
vds_ds4_hw_major_from_report(std::span<const std::uint8_t> report) {
  return read_u16_le(report, 33);
}

std::uint16_t
vds_ds4_hw_minor_from_report(std::span<const std::uint8_t> report) {
  return read_u16_le(report, 35);
}

std::uint32_t
vds_ds4_sw_major_from_report(std::span<const std::uint8_t> report) {
  return read_u32_le(report, 37);
}

std::uint8_t
vds_module_status_from_vendor_report(std::span<const std::uint8_t> report) {
  if (report.size() < 6 || report[0] != kDsVendorGetReport) {
    return 0xff;
  }
  return report[5];
}

std::string vds_controller_info_json(const VdsControllerInfo &info) {
  std::string reply = "{";
  reply += jsonl_bool_field("OK", true);
  reply += ',';
  reply += vds_controller_info_object(info);
  reply += '}';
  return reply;
}

std::string vds_controller_info_object(const VdsControllerInfo &info) {
  std::string reply = "{";
  reply += jsonl_string_field("model", info.model);
  reply += ',';
  reply += jsonl_string_field("connection", info.connection);
  reply += ',';
  reply += jsonl_string_field("serial", info.serial);
  reply += ',';
  reply += jsonl_string_field("firmware", info.firmware);
  reply += ',';
  reply += jsonl_string_field("firmware_version", info.firmware_version);
  reply += ',';
  reply += jsonl_string_field("hardware_version", info.hardware_version);
  reply += ',';
  reply += jsonl_string_field("hardware_model", info.hardware_model);
  reply += ',';
  reply += jsonl_string_field("build_time", info.build_time);
  reply += ',';
  reply += jsonl_string_field("color_code", info.color_code);
  reply += ',';
  reply += jsonl_string_field("color_name", info.color_name);
  reply += ',';
  reply += jsonl_string_field("mac_address", info.mac_address);
  reply += ',';
  reply += jsonl_string_field("left_module", info.left_module);
  reply += ',';
  reply += jsonl_string_field("right_module", info.right_module);
  reply += ',';
  reply += jsonl_bool_field("info_read", info.info_read);
  reply += ',';
  reply += jsonl_bool_field("is_clone", info.is_clone);
  reply += ',';
  reply += jsonl_string_field("error", info.error);
  reply += '}';
  return reply;
}

} // namespace vds
