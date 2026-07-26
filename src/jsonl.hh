// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>
#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vds {

struct JsonlValue {
  std::string string_value;
  std::vector<unsigned> array_value;
  bool is_array = false;
};

struct JsonlField {
  std::string key;
  JsonlValue value;
};

std::vector<JsonlField> parse_jsonl_object(std::string_view text,
                                           std::string_view context);
std::string jsonl_bool_field(std::string_view key, bool value);
std::string jsonl_string_field(std::string_view key, std::string_view value);
std::string jsonl_string_value(std::string_view value);
std::string jsonl_unsigned_array_field(std::string_view key,
                                       std::span<const unsigned> values);
std::string require_jsonl_string(std::span<const JsonlField> fields,
                                 std::string_view key,
                                 std::string_view context);
std::vector<unsigned>
require_jsonl_unsigned_array(std::span<const JsonlField> fields,
                             std::string_view key, std::string_view context);
void reject_unknown_jsonl_fields(std::span<const JsonlField> fields,
                                 std::span<const std::string_view> expected,
                                 std::string_view context);

} // namespace vds
