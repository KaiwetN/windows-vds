// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>
#pragma once

#include <utility>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace vds::win {

class UniqueHandle {
public:
  UniqueHandle() = default;
  explicit UniqueHandle(HANDLE handle) : handle_(handle) {}
  ~UniqueHandle() { reset(); }

  UniqueHandle(const UniqueHandle &) = delete;
  UniqueHandle &operator=(const UniqueHandle &) = delete;

  UniqueHandle(UniqueHandle &&other) noexcept
      : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}

  UniqueHandle &operator=(UniqueHandle &&other) noexcept {
    if (this != &other) {
      reset();
      handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
    }
    return *this;
  }

  HANDLE get() const { return handle_; }
  explicit operator bool() const {
    return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr;
  }

  void reset(HANDLE handle = INVALID_HANDLE_VALUE) {
    if (handle_ == handle) {
      return;
    }
    if (*this) {
      CloseHandle(handle_);
    }
    handle_ = handle;
  }

private:
  HANDLE handle_ = INVALID_HANDLE_VALUE;
};

} // namespace vds::win
