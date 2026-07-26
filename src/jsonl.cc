// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "jsonl.hh"

namespace vds {

namespace {

std::string context_suffix(std::string_view context) {
  if (context.empty()) {
    return {};
  }
  std::string suffix = " in ";
  suffix += context;
  return suffix;
}

void skip_jsonl_space(std::string_view text, std::size_t &offset) {
  while (offset < text.size() &&
         std::isspace(static_cast<unsigned char>(text[offset])) != 0) {
    ++offset;
  }
}

void require_jsonl_char(std::string_view text, std::size_t &offset,
                        char expected, std::string_view context) {
  skip_jsonl_space(text, offset);
  if (offset >= text.size() || text[offset] != expected) {
    throw std::runtime_error("invalid JSON object" + context_suffix(context));
  }
  ++offset;
}

char parse_jsonl_escape(char escape, std::string_view context) {
  switch (escape) {
  case '"':
  case '\\':
  case '/':
    return escape;
  case 'b':
    return '\b';
  case 'f':
    return '\f';
  case 'n':
    return '\n';
  case 'r':
    return '\r';
  case 't':
    return '\t';
  default:
    throw std::runtime_error("unsupported JSON escape" +
                             context_suffix(context));
  }
}

int parse_jsonl_hex_digit(char ch, std::string_view context) {
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }
  if (ch >= 'a' && ch <= 'f') {
    return ch - 'a' + 10;
  }
  if (ch >= 'A' && ch <= 'F') {
    return ch - 'A' + 10;
  }
  throw std::runtime_error("invalid JSON unicode escape" +
                           context_suffix(context));
}

std::string parse_jsonl_string(std::string_view text, std::size_t &offset,
                               std::string_view context) {
  skip_jsonl_space(text, offset);
  if (offset >= text.size() || text[offset] != '"') {
    throw std::runtime_error("expected JSON string" + context_suffix(context));
  }
  ++offset;

  std::string value;
  while (offset < text.size()) {
    const char ch = text[offset++];
    if (ch == '"') {
      return value;
    }
    if (ch == '\\') {
      if (offset >= text.size()) {
        throw std::runtime_error("unterminated JSON escape" +
                                 context_suffix(context));
      }
      const char escape = text[offset++];
      if (escape == 'u') {
        if (offset + 4 > text.size()) {
          throw std::runtime_error("unterminated JSON unicode escape" +
                                   context_suffix(context));
        }
        unsigned int code_point = 0;
        for (int i = 0; i < 4; ++i) {
          code_point = (code_point << 4) |
                       parse_jsonl_hex_digit(text[offset++], context);
        }
        if (code_point > 0x7f) {
          throw std::runtime_error("unsupported JSON unicode escape" +
                                   context_suffix(context));
        }
        value.push_back(static_cast<char>(code_point));
      } else {
        value.push_back(parse_jsonl_escape(escape, context));
      }
      continue;
    }
    if (static_cast<unsigned char>(ch) < 0x20) {
      throw std::runtime_error("invalid control character in JSON string" +
                               context_suffix(context));
    }
    value.push_back(ch);
  }

  throw std::runtime_error("unterminated JSON string" +
                           context_suffix(context));
}

unsigned parse_jsonl_unsigned(std::string_view text, std::size_t &offset,
                              std::string_view context) {
  skip_jsonl_space(text, offset);
  const std::size_t start = offset;
  while (offset < text.size() &&
         std::isdigit(static_cast<unsigned char>(text[offset])) != 0) {
    ++offset;
  }
  if (start == offset) {
    throw std::runtime_error("expected JSON unsigned integer" +
                             context_suffix(context));
  }

  unsigned long long value = 0;
  for (const char ch : text.substr(start, offset - start)) {
    value = value * 10 + static_cast<unsigned>(ch - '0');
    if (value > std::numeric_limits<unsigned>::max()) {
      throw std::runtime_error("JSON unsigned integer overflow" +
                               context_suffix(context));
    }
  }
  return static_cast<unsigned>(value);
}

std::vector<unsigned> parse_jsonl_unsigned_array(std::string_view text,
                                                 std::size_t &offset,
                                                 std::string_view context) {
  std::vector<unsigned> values;
  require_jsonl_char(text, offset, '[', context);
  skip_jsonl_space(text, offset);
  if (offset < text.size() && text[offset] == ']') {
    ++offset;
    return values;
  }

  while (true) {
    values.push_back(parse_jsonl_unsigned(text, offset, context));
    skip_jsonl_space(text, offset);
    if (offset >= text.size()) {
      throw std::runtime_error("unterminated JSON array" +
                               context_suffix(context));
    }
    if (text[offset] == ']') {
      ++offset;
      return values;
    }
    if (text[offset] != ',') {
      throw std::runtime_error("expected JSON array separator" +
                               context_suffix(context));
    }
    ++offset;
  }
}

JsonlValue parse_jsonl_value(std::string_view text, std::size_t &offset,
                             std::string_view context) {
  skip_jsonl_space(text, offset);
  if (offset >= text.size()) {
    throw std::runtime_error("missing JSON value" + context_suffix(context));
  }
  if (text[offset] == '"') {
    return JsonlValue{
        .string_value = parse_jsonl_string(text, offset, context),
        .array_value = {},
        .is_array = false,
    };
  }
  if (text[offset] == '[') {
    return JsonlValue{
        .string_value = {},
        .array_value = parse_jsonl_unsigned_array(text, offset, context),
        .is_array = true,
    };
  }
  throw std::runtime_error("unsupported JSON value" + context_suffix(context));
}

} // namespace

std::vector<JsonlField> parse_jsonl_object(std::string_view text,
                                           std::string_view context) {
  std::vector<JsonlField> fields;
  std::size_t offset = 0;

  require_jsonl_char(text, offset, '{', context);
  skip_jsonl_space(text, offset);
  if (offset < text.size() && text[offset] == '}') {
    ++offset;
    skip_jsonl_space(text, offset);
    if (offset != text.size()) {
      throw std::runtime_error("trailing data after JSON object" +
                               context_suffix(context));
    }
    return fields;
  }

  while (true) {
    std::string key = parse_jsonl_string(text, offset, context);
    require_jsonl_char(text, offset, ':', context);
    fields.push_back(JsonlField{
        .key = std::move(key),
        .value = parse_jsonl_value(text, offset, context),
    });

    skip_jsonl_space(text, offset);
    if (offset >= text.size()) {
      throw std::runtime_error("unterminated JSON object" +
                               context_suffix(context));
    }
    if (text[offset] == '}') {
      ++offset;
      skip_jsonl_space(text, offset);
      if (offset != text.size()) {
        throw std::runtime_error("trailing data after JSON object" +
                                 context_suffix(context));
      }
      return fields;
    }
    if (text[offset] != ',') {
      throw std::runtime_error("expected JSON field separator" +
                               context_suffix(context));
    }
    ++offset;
  }
}

namespace {

std::string escape_jsonl_string(std::string_view text) {
  std::string escaped;
  for (const char ch : text) {
    switch (ch) {
    case '"':
      escaped += "\\\"";
      break;
    case '\\':
      escaped += "\\\\";
      break;
    case '\b':
      escaped += "\\b";
      break;
    case '\f':
      escaped += "\\f";
      break;
    case '\n':
      escaped += "\\n";
      break;
    case '\r':
      escaped += "\\r";
      break;
    case '\t':
      escaped += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(ch) < 0x20) {
        char code[7];
        std::snprintf(code, sizeof(code), "\\u%04x",
                      static_cast<unsigned char>(ch));
        escaped += code;
      } else {
        escaped.push_back(ch);
      }
      break;
    }
  }
  return escaped;
}

} // namespace

std::string jsonl_string_field(std::string_view key, std::string_view value) {
  std::string text = "\"";
  text += key;
  text += "\":";
  text += jsonl_string_value(value);
  return text;
}

std::string jsonl_bool_field(std::string_view key, bool value) {
  std::string text = "\"";
  text += key;
  text += "\":";
  text += value ? "true" : "false";
  return text;
}

std::string jsonl_string_value(std::string_view value) {
  std::string text = "\"";
  text += escape_jsonl_string(value);
  text += '"';
  return text;
}

namespace {

std::string jsonl_unsigned_array(std::span<const unsigned> values) {
  std::string text = "[";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i > 0) {
      text += ',';
    }
    text += std::to_string(values[i]);
  }
  text += ']';
  return text;
}

} // namespace

std::string jsonl_unsigned_array_field(std::string_view key,
                                       std::span<const unsigned> values) {
  std::string text = "\"";
  text += key;
  text += "\":";
  text += jsonl_unsigned_array(values);
  return text;
}

namespace {

const JsonlValue *find_jsonl_field(std::span<const JsonlField> fields,
                                   std::string_view key,
                                   std::string_view context) {
  const JsonlValue *value = nullptr;
  for (const auto &field : fields) {
    if (field.key != key) {
      continue;
    }
    if (value != nullptr) {
      throw std::runtime_error("duplicate JSON field" +
                               context_suffix(context) + ": " +
                               std::string(key));
    }
    value = &field.value;
  }
  return value;
}

} // namespace

std::string require_jsonl_string(std::span<const JsonlField> fields,
                                 std::string_view key,
                                 std::string_view context) {
  const JsonlValue *value = find_jsonl_field(fields, key, context);
  if (value == nullptr) {
    throw std::runtime_error("missing JSON field" + context_suffix(context) +
                             ": " + std::string(key));
  }
  if (value->is_array) {
    throw std::runtime_error("JSON field must be a string" +
                             context_suffix(context) + ": " + std::string(key));
  }
  return value->string_value;
}

std::vector<unsigned>
require_jsonl_unsigned_array(std::span<const JsonlField> fields,
                             std::string_view key, std::string_view context) {
  const JsonlValue *value = find_jsonl_field(fields, key, context);
  if (value == nullptr) {
    throw std::runtime_error("missing JSON field" + context_suffix(context) +
                             ": " + std::string(key));
  }
  if (!value->is_array) {
    throw std::runtime_error("JSON field must be an array" +
                             context_suffix(context) + ": " + std::string(key));
  }
  return value->array_value;
}

void reject_unknown_jsonl_fields(std::span<const JsonlField> fields,
                                 std::span<const std::string_view> expected,
                                 std::string_view context) {
  for (const auto &field : fields) {
    if (std::find(expected.begin(), expected.end(), field.key) ==
        expected.end()) {
      throw std::runtime_error("unknown JSON field" + context_suffix(context) +
                               ": " + field.key);
    }
  }
}

} // namespace vds
