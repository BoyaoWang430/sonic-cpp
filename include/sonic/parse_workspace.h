/*
 * Copyright 2026 ByteDance Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstddef>
#include <cstdlib>

namespace sonic_json {
namespace internal {
template <typename NodeType>
class SAXWorkspaceLease;
}  // namespace internal

// Reusable SAX storage; Documents own their DOM and input independently.
// Must outlive active Parse calls. Synchronous reentry is supported;
// concurrent use is not.
class ParseWorkspace {
 public:
  explicit ParseWorkspace(std::size_t max_capacity = 256 * 1024) noexcept
      : max_capacity_(max_capacity) {}

  ParseWorkspace(const ParseWorkspace&) = delete;
  ParseWorkspace& operator=(const ParseWorkspace&) = delete;
  ParseWorkspace(ParseWorkspace&&) = delete;
  ParseWorkspace& operator=(ParseWorkspace&&) = delete;

  ~ParseWorkspace() { Release(); }

  // Retained bytes, excluding storage borrowed by active parses.
  std::size_t Capacity() const noexcept { return capacity_; }
  std::size_t MaxCapacity() const noexcept { return max_capacity_; }

  // Active parses may return storage after Release().
  void Release() noexcept {
    if (data_) std::free(data_);
    data_ = nullptr;
    capacity_ = 0;
  }

 private:
  template <typename NodeType>
  friend class internal::SAXWorkspaceLease;

  void* data_{nullptr};
  std::size_t capacity_{0};
  const std::size_t max_capacity_;
};

}  // namespace sonic_json
