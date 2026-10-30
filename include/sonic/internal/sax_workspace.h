/*
 * Copyright 2026 ByteDance Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstddef>
#include <cstdlib>
#include <limits>

#include "sonic/dom/handler.h"
#include "sonic/parse_workspace.h"

namespace sonic_json {
namespace internal {

// Isolate workspace overflow and exception handling from the default SAX path.
template <typename NodeType>
class SAXWorkspaceHandler : public SAXHandler<NodeType> {
 public:
  using Base = SAXHandler<NodeType>;
  using Allocator = typename Base::Allocator;
  using MemberType = typename Base::MemberType;

  explicit SAXWorkspaceHandler(Allocator& alloc) : Base(alloc) {}

  sonic_force_inline bool Null() noexcept {
    return canAddNode() && Base::Null();
  }

  sonic_force_inline bool Bool(bool val) noexcept {
    return canAddNode() && Base::Bool(val);
  }

  sonic_force_inline bool Uint(uint64_t val) noexcept {
    return canAddNode() && Base::Uint(val);
  }

  sonic_force_inline bool Int(int64_t val) noexcept {
    return canAddNode() && Base::Int(val);
  }

  sonic_force_inline bool Double(double val) noexcept {
    return canAddNode() && Base::Double(val);
  }

  sonic_force_inline bool String(StringView s) {
    return canAddNode() && Base::String(s);
  }

  sonic_force_inline bool NumStr(StringView s) {
    return canAddNode() && Base::NumStr(s);
  }

  sonic_force_inline bool Raw(const char* data, size_t len) {
    return canAddNode() && Base::Raw(data, len);
  }

  sonic_force_inline bool Key(StringView s) {
    return canAddNode() && Base::Key(s);
  }

  sonic_force_inline bool StartObject() noexcept {
    return canAddNode() && Base::StartObject();
  }

  sonic_force_inline bool StartArray() noexcept {
    return canAddNode() && Base::StartArray();
  }

  sonic_force_inline bool EndObject(uint32_t pairs) {
    NodeType& obj = this->st_[this->parent_];
    size_t old = obj.o.next.ofs;
    if (pairs) {
      void* mem =
          obj.template containerMalloc<MemberType>(pairs, *this->alloc_);
      obj.setLength(pairs, kObject);
      if (sonic_unlikely(mem == nullptr)) {
        NodeType* children = &obj + 1;
        for (size_t i = 0; i < size_t(pairs) * 2; i++) children[i].~NodeType();
        obj.setLength(0, kObject);
        obj.setChildren(nullptr);
        this->oom_ = true;
      } else {
        obj.setChildren(mem);
        internal::Xmemcpy<sizeof(MemberType)>(
            (void*)obj.getObjChildrenFirstUnsafe(), (void*)(&obj + 1), pairs);
      }
    } else {
      obj.setLength(0, kObject);
      obj.setChildren(nullptr);
    }
    this->np_ = this->parent_ + 1;
    this->parent_ = old;
    return true;
  }

  sonic_force_inline bool EndArray(uint32_t count) {
    NodeType& arr = this->st_[this->parent_];
    size_t old = arr.o.next.ofs;
    if (count) {
      void* mem = arr.template containerMalloc<NodeType>(count, *this->alloc_);
      arr.setLength(count, kArray);
      if (sonic_unlikely(mem == nullptr)) {
        NodeType* children = &arr + 1;
        for (size_t i = 0; i < count; i++) children[i].~NodeType();
        arr.setLength(0, kArray);
        arr.setChildren(nullptr);
        this->oom_ = true;
      } else {
        arr.setChildren(mem);
        internal::Xmemcpy<sizeof(NodeType)>(
            (void*)arr.getArrChildrenFirstUnsafe(), (void*)(&arr + 1), count);
      }
    } else {
      arr.setLength(0, kArray);
      arr.setChildren(nullptr);
    }
    this->np_ = this->parent_ + 1;
    this->parent_ = old;
    return true;
  }

 private:
  sonic_force_inline bool canAddNode() noexcept {
    if (sonic_likely(this->np_ < this->cap_)) return true;
    if (sonic_unlikely(this->cap_ > std::numeric_limits<std::size_t>::max() /
                                        sizeof(NodeType) / 2)) {
      this->oom_ = true;
      return false;
    }
    return true;
  }
};

// Borrows raw storage for a fresh handler and destroys its nodes before return.
template <typename NodeType>
class SAXWorkspaceLease {
 public:
  SAXWorkspaceLease(SAXHandler<NodeType>& sax,
                    ParseWorkspace& workspace) noexcept
      : sax_(sax), workspace_(workspace) {}

  SAXWorkspaceLease(const SAXWorkspaceLease&) = delete;
  SAXWorkspaceLease& operator=(const SAXWorkspaceLease&) = delete;
  SAXWorkspaceLease(SAXWorkspaceLease&&) = delete;
  SAXWorkspaceLease& operator=(SAXWorkspaceLease&&) = delete;

  ~SAXWorkspaceLease() {
    if (!sax_.st_) return;
    for (std::size_t i = 0; i < sax_.np_; ++i) {
      sax_.st_[i].~NodeType();
    }
    void* data = sax_.st_;
    const std::size_t bytes = sax_.cap_ == initial_nodes_
                                  ? initial_bytes_
                                  : sax_.cap_ * sizeof(NodeType);
    sax_.st_ = nullptr;
    sax_.np_ = 0;
    sax_.cap_ = 0;
    sax_.parent_ = 0;
    if (bytes <= workspace_.max_capacity_ && bytes > workspace_.capacity_) {
      workspace_.Release();
      workspace_.data_ = data;
      workspace_.capacity_ = bytes;
    } else {
      std::free(data);
    }
  }

  bool SetUp(StringView json) noexcept {
    std::size_t nodes = json.size() / 2 + 2;
    if (nodes < 16) nodes = 16;
    if (nodes > std::numeric_limits<std::size_t>::max() / sizeof(NodeType)) {
      return false;
    }
    const std::size_t bytes = nodes * sizeof(NodeType);
    // Leave the idle block available to nested parses for oversized requests.
    if (bytes <= workspace_.max_capacity_ && workspace_.data_) {
      sax_.st_ = static_cast<NodeType*>(workspace_.data_);
      sax_.cap_ = workspace_.capacity_ / sizeof(NodeType);
      initial_bytes_ = workspace_.capacity_;
      initial_nodes_ = sax_.cap_;
      workspace_.data_ = nullptr;
      workspace_.capacity_ = 0;
    }
    if (!sax_.SetUp(json)) return false;
    // Preserve residual bytes when a different NodeType reuses the allocation.
    if (sax_.cap_ != initial_nodes_) {
      initial_nodes_ = sax_.cap_;
      initial_bytes_ = sax_.cap_ * sizeof(NodeType);
    }
    return true;
  }

 private:
  SAXHandler<NodeType>& sax_;
  ParseWorkspace& workspace_;
  std::size_t initial_bytes_{0};
  std::size_t initial_nodes_{0};
};

}  // namespace internal
}  // namespace sonic_json
