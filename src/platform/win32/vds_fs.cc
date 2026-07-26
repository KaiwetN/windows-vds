// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include <windows.h>

#include "vds_fs.hh"
#include "vds_win32.hh"

namespace {

using vds::win::win32_error_message;

class WindowsFileLock final : public vds::FileLock {
public:
  explicit WindowsFileLock(HANDLE handle) : handle_(handle) {}

  ~WindowsFileLock() override {
    if (handle_ == INVALID_HANDLE_VALUE) {
      return;
    }
    OVERLAPPED overlapped{};
    UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &overlapped);
    CloseHandle(handle_);
  }

private:
  HANDLE handle_ = INVALID_HANDLE_VALUE;
};

void write_all(HANDLE handle, std::string_view text,
               const std::filesystem::path &path) {
  const char *data = text.data();
  std::size_t size = text.size();
  while (size > 0) {
    const DWORD chunk =
        static_cast<DWORD>(std::min<std::size_t>(size, MAXDWORD));
    DWORD written = 0;
    if (!WriteFile(handle, data, chunk, &written, nullptr)) {
      throw std::runtime_error("failed to write " + path.string() + ": " +
                               win32_error_message(GetLastError()));
    }
    if (written == 0) {
      throw std::runtime_error("write returned zero for " + path.string());
    }
    data += written;
    size -= written;
  }
}

} // namespace

namespace vds {

using win::win32_error_message;

void ensure_parent_directory(const std::filesystem::path &path) {
  const std::filesystem::path directory = path.parent_path();
  if (!directory.empty()) {
    std::filesystem::create_directories(directory);
  }
}

std::unique_ptr<FileLock> lock_file(const std::filesystem::path &path) {
  const std::filesystem::path lock_path = path.string() + ".lock";
  HANDLE handle = CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    throw std::runtime_error("failed to open DB lock file " +
                             lock_path.string() + ": " +
                             win32_error_message(GetLastError()));
  }

  OVERLAPPED overlapped{};
  if (!LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD,
                  &overlapped)) {
    const DWORD error = GetLastError();
    CloseHandle(handle);
    throw std::runtime_error("failed to lock DB file " + lock_path.string() +
                             ": " + win32_error_message(error));
  }
  return std::make_unique<WindowsFileLock>(handle);
}

std::string read_file_if_exists(const std::filesystem::path &path) {
  std::error_code error;
  if (!std::filesystem::exists(path, error)) {
    if (!error) {
      return {};
    }
    throw std::runtime_error("failed to stat " + path.string() + ": " +
                             error.message());
  }

  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("failed to open " + path.string());
  }
  return std::string(std::istreambuf_iterator<char>(file),
                     std::istreambuf_iterator<char>());
}

void write_file_atomic(const std::filesystem::path &path,
                       std::string_view text) {
  const std::filesystem::path tmp_path = path.string() + ".tmp";
  HANDLE handle = CreateFileW(tmp_path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    throw std::runtime_error("failed to open " + tmp_path.string() + ": " +
                             win32_error_message(GetLastError()));
  }

  try {
    write_all(handle, text, tmp_path);
    if (!FlushFileBuffers(handle)) {
      throw std::runtime_error("failed to flush " + tmp_path.string() + ": " +
                               win32_error_message(GetLastError()));
    }
  } catch (...) {
    CloseHandle(handle);
    DeleteFileW(tmp_path.c_str());
    throw;
  }
  CloseHandle(handle);

  if (!MoveFileExW(tmp_path.c_str(), path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    const DWORD error = GetLastError();
    DeleteFileW(tmp_path.c_str());
    throw std::runtime_error("failed to rename " + tmp_path.string() + " to " +
                             path.string() + ": " + win32_error_message(error));
  }
}

} // namespace vds
