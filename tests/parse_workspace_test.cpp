/*
 * Copyright 2026 ByteDance Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sonic/parse_workspace.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "sonic/sonic.h"

namespace {
using namespace sonic_json;

std::string WorkspaceDense(size_t count) {
  std::string result = "[";
  for (size_t i = 0; i < count; ++i) {
    if (i) result += ',';
    result += std::to_string(i % 97);
  }
  return result + ']';
}
std::string WorkspaceNested(size_t depth) {
  std::string result = R"({"leaf":"escaped\nvalue","empty":[]})";
  for (size_t i = 0; i < depth; ++i) {
    result = i & 1 ? "[0," + result + ",{}]"
                   : "{\"before\":[],\"child\":" + result + ",\"after\":1}";
  }
  return result;
}

template <typename Allocator>
void WorkspaceLifetime() {
  using Doc = GenericDocument<DNode<Allocator>>;
  std::vector<std::unique_ptr<Doc>> documents;
  const std::vector<std::string> inputs = {
      R"({"key":"escaped\nvalue","array":[1,{},[],true]})",
      WorkspaceDense(4097),
      WorkspaceNested(65),
      "{}",
      "[]",
      "null"};
  std::string saved;
  const char* string_reference = nullptr;
  {
    ParseWorkspace workspace;
    for (const auto& input : inputs) {
      auto document = std::make_unique<Doc>();
      document->Parse(input, workspace);
      ASSERT_FALSE(document->HasParseError());
      EXPECT_EQ(document->Dump(), input);
      documents.push_back(std::move(document));
    }
    auto view = (*documents[0])["key"].GetStringView();
    string_reference = view.data();
    saved.assign(view.data(), view.size());
    ASSERT_GT(workspace.Capacity(), 0u);
    workspace.Release();
    EXPECT_EQ(workspace.Capacity(), 0u);
    EXPECT_EQ(workspace.MaxCapacity(), 256u * 1024u);
    EXPECT_EQ(std::string(string_reference, saved.size()), saved);
    Doc replacement;
    replacement.Parse(R"({"replacement":"different\tbytes"})", workspace);
    ASSERT_FALSE(replacement.HasParseError());
    EXPECT_EQ(std::string(string_reference, saved.size()), saved);
    for (size_t i = 0; i < inputs.size(); ++i)
      EXPECT_EQ(documents[i]->Dump(), inputs[i]);
  }
  EXPECT_EQ(std::string(string_reference, saved.size()), saved);
  for (size_t i = 0; i < inputs.size(); ++i)
    EXPECT_EQ(documents[i]->Dump(), inputs[i]);
}
TEST(ParseWorkspace, FreshDocumentsOwnDomBeyondWorkspaceLifetime) {
  WorkspaceLifetime<MemoryPoolAllocator<>>();
  WorkspaceLifetime<SimpleAllocator>();
}

TEST(ParseWorkspace, ExplicitLimitsReleaseAndOversizedInput) {
  {
    ParseWorkspace workspace;
    Document document;
    document.Parse("[]", workspace);
    ASSERT_FALSE(document.HasParseError());
    const size_t retained = workspace.Capacity();
    const size_t largest = std::numeric_limits<size_t>::max();
    for (size_t length : {largest, largest - 64}) {
      document.Parse("null", length, workspace);
      EXPECT_EQ(document.GetParseError(), kErrorNoMem);
      EXPECT_TRUE(document.IsNull());
      EXPECT_EQ(workspace.Capacity(), retained);
    }
    document.Parse(R"({"recovered":true})", workspace);
    ASSERT_FALSE(document.HasParseError());
    EXPECT_TRUE(document["recovered"].GetBool());
  }
  const std::string large = "\"" + std::string(65537, 'x') + "\"";
  for (size_t limit : {size_t(0), size_t(255), size_t(256), size_t(64 * 1024),
                       size_t(256 * 1024), size_t(1024 * 1024)}) {
    SCOPED_TRACE(limit);
    ParseWorkspace workspace(limit);
    ASSERT_EQ(workspace.Capacity(), 0u);
    ASSERT_EQ(workspace.MaxCapacity(), limit);
    Document small, big, later;
    small.Parse(R"({"key":"still alive"})", workspace);
    ASSERT_FALSE(small.HasParseError());
    const size_t retained = workspace.Capacity();
    EXPECT_LE(retained, limit);
    if (limit < 256) EXPECT_EQ(retained, 0u);
    big.Parse(large, workspace);
    ASSERT_FALSE(big.HasParseError());
    EXPECT_EQ(big.Dump(), large);
    EXPECT_LE(workspace.Capacity(), limit);
    if (limit <= 256 * 1024) EXPECT_EQ(workspace.Capacity(), retained);
    later.Parse("[]", 2, workspace);
    ASSERT_FALSE(later.HasParseError());
    EXPECT_EQ(small["key"].GetStringView(), "still alive");
    workspace.Release();
    workspace.Release();
    EXPECT_EQ(workspace.Capacity(), 0u);
    EXPECT_EQ(big.Dump(), large);
    EXPECT_EQ(small.Dump(), R"({"key":"still alive"})");
  }
}

template <typename Allocator, ParseFlags Flags>
void CompareWorkspaceFlags(ParseWorkspace& workspace) {
  using Doc = GenericDocument<DNode<Allocator>>;
  const std::vector<std::string> inputs = {
      "{}",
      "[]",
      "null",
      "true",
      "-1",
      "18446744073709551615",
      "18446744073709551616",
      std::string(256, '9'),
      "1e9999",
      "0.12345678901234567",
      R"({"n":18446744073709551616,"v":"end\n","arr":[-0,1.25]})",
      std::string("\"line\nzero\0tail\"", 16),
      "",
      "{",
      "[1,",
      R"({"key" 1})",
      R"({"key":"dangling\)",
      R"("\uD800")",
      R"({"a":[{},[],"ok"],"b":true})"};
  Doc expected, actual;
  for (size_t i = 0; i < inputs.size(); ++i) {
    SCOPED_TRACE(::testing::Message()
                 << "flags=" << static_cast<unsigned>(Flags) << " index=" << i);
    const auto& input = inputs[i];
    expected.template Parse<Flags>(input);
    if (i & 1)
      actual.template Parse<Flags>(input.data(), input.size(), workspace);
    else
      actual.template Parse<Flags>(StringView(input.data(), input.size()),
                                   workspace);
    ASSERT_EQ(actual.GetParseError(), expected.GetParseError());
    EXPECT_EQ(actual.GetErrorOffset(), expected.GetErrorOffset());
    if (!expected.HasParseError()) {
      EXPECT_EQ(actual.GetType(), expected.GetType());
      EXPECT_EQ(actual.Dump(), expected.Dump());
    }
    EXPECT_LE(workspace.Capacity(), workspace.MaxCapacity());
  }
}
template <typename Allocator>
void AllWorkspaceFlags(ParseWorkspace& workspace) {
  CompareWorkspaceFlags<Allocator, static_cast<ParseFlags>(0)>(workspace);
  CompareWorkspaceFlags<Allocator, static_cast<ParseFlags>(2)>(workspace);
  CompareWorkspaceFlags<Allocator, static_cast<ParseFlags>(4)>(workspace);
  CompareWorkspaceFlags<Allocator, static_cast<ParseFlags>(6)>(workspace);
  CompareWorkspaceFlags<Allocator, static_cast<ParseFlags>(8)>(workspace);
  CompareWorkspaceFlags<Allocator, static_cast<ParseFlags>(10)>(workspace);
  CompareWorkspaceFlags<Allocator, static_cast<ParseFlags>(12)>(workspace);
  CompareWorkspaceFlags<Allocator, static_cast<ParseFlags>(14)>(workspace);
}
TEST(ParseWorkspace, SameWorkspaceSupportsAllocatorTypesFlagsAndErrors) {
  ParseWorkspace workspace;
  AllWorkspaceFlags<MemoryPoolAllocator<>>(workspace);
  AllWorkspaceFlags<SimpleAllocator>(workspace);
  CompareWorkspaceFlags<
      MemoryPoolAllocator<SimpleAllocator, AdaptiveChunkPolicy>,
      ParseFlags::kParseDefault>(workspace);
}

template <typename Allocator>
void WorkspaceDocumentMoves() {
  using Doc = GenericDocument<DNode<Allocator>>;
  std::optional<Doc> first;
  Doc target;
  const std::string a = R"({"a":["one\n",{},[]]})";
  const std::string b = R"({"b":[1,2,3]})";
  {
    ParseWorkspace source_workspace;
    Doc source;
    source.Parse(a, source_workspace);
    ASSERT_FALSE(source.HasParseError());
    first.emplace(std::move(source));
    source_workspace.Release();
    EXPECT_EQ(first->Dump(), a);
  }
  {
    ParseWorkspace target_workspace;
    target.Parse(b, target_workspace);
    ASSERT_FALSE(target.HasParseError());
    target = std::move(*first);
    first.reset();
    EXPECT_EQ(target.Dump(), a);
    Doc second;
    second.Parse(b, target_workspace);
    target.Swap(second);
    EXPECT_EQ(second.Dump(), a);
    EXPECT_EQ(target.Dump(), b);
  }
  EXPECT_EQ(target.Dump(), b);
}
TEST(ParseWorkspace, DocumentMovesAndSwapOutliveDifferentWorkspaces) {
  WorkspaceDocumentMoves<MemoryPoolAllocator<>>();
  WorkspaceDocumentMoves<SimpleAllocator>();
}

struct WorkspaceTrackingAllocator {
  static constexpr bool kNeedFree = true;
  inline static ptrdiff_t live = 0;
  size_t calls = 0, fail_at = 0;
  void* Malloc(size_t size) {
    if (!size) return nullptr;
    if (++calls == fail_at) return nullptr;
    void* p = std::malloc(size);
    if (p) ++live;
    return p;
  }
  void* Realloc(void* old, size_t, size_t size) {
    if (!size) {
      Free(old);
      return nullptr;
    }
    if (++calls == fail_at) return nullptr;
    void* p = std::realloc(old, size);
    if (p && !old) ++live;
    return p;
  }
  static void Free(void* p) {
    if (p) --live;
    std::free(p);
  }
};
TEST(ParseWorkspace, WarmWorkspaceDoesNotHideDomAllocatorFailure) {
  using Doc = GenericDocument<DNode<WorkspaceTrackingAllocator>>;
  const std::string input =
      R"({"a":[{"value":"one"},{"value":"two"}],"b":[1,2,3],"c":{"d":[]}})";
  for (bool warm : {false, true}) {
    size_t count = 0;
    {
      ParseWorkspace workspace;
      WorkspaceTrackingAllocator allocator;
      Doc document(&allocator);
      document.Parse(input, workspace);
      ASSERT_FALSE(document.HasParseError());
      count = allocator.calls;
    }
    for (size_t fail = 1; fail <= count; ++fail) {
      SCOPED_TRACE(::testing::Message() << "warm=" << warm << " fail=" << fail);
      {
        ParseWorkspace workspace;
        WorkspaceTrackingAllocator allocator;
        if (warm) {
          Doc document(&allocator);
          document.Parse(input, workspace);
          ASSERT_FALSE(document.HasParseError());
        }
        allocator.calls = 0;
        allocator.fail_at = fail;
        Doc document(&allocator);
        document.Parse(input, workspace);
        EXPECT_EQ(document.GetParseError(), kErrorNoMem);
        allocator.fail_at = 0;
        document.Parse(R"({"recovered":true})", workspace);
        ASSERT_FALSE(document.HasParseError());
        EXPECT_TRUE(document["recovered"].GetBool());
      }
      EXPECT_EQ(WorkspaceTrackingAllocator::live, 0);
    }
  }
}

struct WorkspaceReentrantAllocator {
  static constexpr bool kNeedFree = true;
  ParseWorkspace* workspace = nullptr;
  bool armed = false, entered = false, nested_ok = false;
  void* Malloc(size_t size) {
    if (armed) {
      armed = false;
      entered = true;
      WorkspaceReentrantAllocator nested_allocator;
      GenericDocument<DNode<WorkspaceReentrantAllocator>> nested(
          &nested_allocator);
      nested.Parse(R"({"nested":[1,{},[],"ok\n"]})", *workspace);
      nested_ok = !nested.HasParseError() &&
                  nested.Dump() == R"({"nested":[1,{},[],"ok\n"]})";
      workspace->Release();
      nested_ok = nested_ok && nested["nested"][3].GetStringView() == "ok\n";
    }
    return size ? std::malloc(size) : nullptr;
  }
  void* Realloc(void* p, size_t, size_t size) {
    if (!size) {
      std::free(p);
      return nullptr;
    }
    return std::realloc(p, size);
  }
  static void Free(void* p) { std::free(p); }
};
TEST(ParseWorkspace, ReentrantAllocatorCanBorrowSameWorkspace) {
  for (bool warm : {false, true}) {
    ParseWorkspace workspace;
    if (warm) {
      Document document;
      document.Parse("[]", workspace);
      ASSERT_FALSE(document.HasParseError());
    }
    WorkspaceReentrantAllocator allocator;
    allocator.workspace = &workspace;
    allocator.armed = true;
    GenericDocument<DNode<WorkspaceReentrantAllocator>> outer(&allocator);
    const std::string input = R"({"outer":[{},"text\n",[1,2]],"tail":true})";
    outer.Parse(input, workspace);
    ASSERT_FALSE(outer.HasParseError());
    EXPECT_TRUE(allocator.entered);
    EXPECT_TRUE(allocator.nested_ok);
    EXPECT_EQ(outer.Dump(), input);
    EXPECT_LE(workspace.Capacity(), workspace.MaxCapacity());
  }
}

#if defined(__cpp_exceptions)
struct WorkspaceThrowingAllocator : WorkspaceTrackingAllocator {
  size_t throw_at = 0;
  void* Malloc(size_t size) {
    if (!size) return nullptr;
    if (++calls == throw_at) throw std::bad_alloc();
    void* p = std::malloc(size);
    if (p) ++live;
    return p;
  }
};
TEST(ParseWorkspace, ThrowingAllocatorUnwindsLiveNodesAndAllowsRecovery) {
  using Doc = GenericDocument<DNode<WorkspaceThrowingAllocator>>;
  const std::string input =
      R"({"a":[{"x":"first"},{"y":[1,2]}],"b":{"nested":true}})";
  size_t count = 0;
  {
    ParseWorkspace workspace;
    WorkspaceThrowingAllocator allocator;
    Doc document(&allocator);
    document.Parse(input, workspace);
    ASSERT_FALSE(document.HasParseError());
    count = allocator.calls;
  }
  ASSERT_GT(count, 1u);
  for (bool warm : {false, true})
    for (size_t fail = 1; fail <= count; ++fail) {
      SCOPED_TRACE(::testing::Message()
                   << "warm=" << warm << " throw=" << fail);
      {
        ParseWorkspace workspace;
        WorkspaceThrowingAllocator allocator;
        if (warm) {
          Doc warm_document(&allocator);
          warm_document.Parse(input, workspace);
          ASSERT_FALSE(warm_document.HasParseError());
        }
        allocator.calls = 0;
        allocator.throw_at = fail;
        Doc document(&allocator);
        EXPECT_THROW(document.Parse(input, workspace), std::bad_alloc);
        EXPECT_LE(workspace.Capacity(), workspace.MaxCapacity());
        allocator.throw_at = 0;
        document.Parse(R"({"recovered":[true,{},[]]})", workspace);
        ASSERT_FALSE(document.HasParseError());
        EXPECT_EQ(document.Dump(), R"({"recovered":[true,{},[]]})");
      }
      EXPECT_EQ(WorkspaceTrackingAllocator::live, 0);
    }
}
#endif
}  // namespace
