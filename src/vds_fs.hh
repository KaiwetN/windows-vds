// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace vds {

class FileLock {
public:
  virtual ~FileLock() = default;

protected:
  FileLock() = default;
};

void ensure_parent_directory(const std::filesystem::path &path);
std::unique_ptr<FileLock> lock_file(const std::filesystem::path &path);
std::string read_file_if_exists(const std::filesystem::path &path);
void write_file_atomic(const std::filesystem::path &path,
                       std::string_view text);

} // namespace vds
