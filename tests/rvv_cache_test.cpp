/*
 * Copyright 2026 ByteDance Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "sonic/sonic.h"

#if defined(SONIC_HAVE_RVV)
#include <gtest/gtest.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {
using namespace sonic_json;
namespace rvv = sonic_json::internal::rvv;

uint8_t SpaceOracle(const uint8_t* data, size_t& pos, size_t len) {
  while (pos < len) {
    const uint8_t ch = data[pos++];
    if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') return ch;
  }
  return pos ? data[pos - 1] : 0;
}

TEST(RvvCache, RepeatedScansAndTokenJumps) {
  for (size_t len = 0; len <= 259; ++len) {
    std::vector<uint8_t> input(len);
    for (size_t i = 0; i < len; ++i) {
      static const uint8_t pattern[] = {' ', '\n', 'x', '\t',
                                        ',', 0, '\r', 0xff};
      input[i] = pattern[(i * 17 + i / 19) % sizeof(pattern)];
    }
    for (size_t jump :
         {size_t{0}, size_t{1}, size_t{3}, size_t{17}, size_t{63}}) {
      size_t pos = 0, expected = 0, end = 0;
      uint64_t bits = 0;
      do {
        const uint8_t want = SpaceOracle(input.data(), expected, len);
        EXPECT_EQ(rvv::skip_space_safe(input.data(), pos, len, end, bits),
                  want);
        ASSERT_EQ(pos, expected) << "len=" << len << " jump=" << jump;
        pos = expected = std::min(len, pos + jump);
      } while (pos < len);
    }
  }
}

TEST(RvvCache, PeekAndAdvanceKeepTheSameToken) {
  const std::string input = " \n[\t\r" + std::string(81, ' ') + " 1 , true ]";
  internal::SkipScanner2 scanner;
  scanner.data_ = reinterpret_cast<const uint8_t*>(input.data());
  scanner.len_ = input.size();
  size_t expected = 0;
  while (expected < input.size()) {
    const uint8_t want = SpaceOracle(scanner.data_, expected, input.size());
    EXPECT_EQ(scanner.peek(), want);
    EXPECT_EQ(scanner.peek(), want);
    EXPECT_EQ(scanner.advance(), want);
    EXPECT_EQ(scanner.pos_, expected);
  }
  EXPECT_FALSE(scanner.hasError());
}

TEST(RvvCache, SafeAndPaddedScansShareOneCursor) {
  std::string input;
  for (size_t i = 0; i < 38; ++i) input += std::string(i % 23, ' ') + 'x';
  const size_t len = input.size();
  input.append(16, '\x01');
  const auto* data = reinterpret_cast<const uint8_t*>(input.data());
  size_t pos = 0, expected = 0, end = 0, calls = 0;
  uint64_t bits = 0;
  while (pos < len) {
    const uint8_t want = SpaceOracle(data, expected, len);
    const uint8_t got =
        calls++ % 2 ? rvv::skip_space(data, pos, end, bits)
                    : rvv::skip_space_safe(data, pos, len, end, bits);
    EXPECT_EQ(got, want);
    ASSERT_EQ(pos, expected);
  }
}

TEST(RvvCache, EmptySuffixAndAllWhitespaceKeepLegacyResults) {
  for (size_t len = 0; len < 194; ++len) {
    const std::string input(len, '\t');
    const auto* data = reinterpret_cast<const uint8_t*>(input.data());
    size_t pos = 0, end = 0;
    uint64_t bits = 0;
    EXPECT_EQ(rvv::skip_space_safe(data, pos, len, end, bits), len ? '\t' : 0);
    EXPECT_EQ(pos, len);
    EXPECT_EQ(rvv::skip_space_safe(data, pos, len, end, bits), len ? '\t' : 0);
    EXPECT_EQ(pos, len);
  }
}

#if defined(__linux__)
TEST(RvvCache, BoundedCacheAndTailStopAtGuardPage) {
  const long page_size = sysconf(_SC_PAGESIZE);
  ASSERT_GT(page_size, 0);
  const size_t page = static_cast<size_t>(page_size);
  void* mapping = mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  ASSERT_NE(mapping, MAP_FAILED);
  ASSERT_EQ(mprotect(static_cast<char*>(mapping) + page, page, PROT_NONE), 0);
  for (size_t len = 0; len <= 195; ++len) {
    auto* data = static_cast<uint8_t*>(mapping) + page - len;
    for (size_t pattern = 0; pattern < 5; ++pattern) {
      SCOPED_TRACE("len=" + std::to_string(len) +
                   " pattern=" + std::to_string(pattern));
      std::fill(data, data + len, ' ');
      if (pattern == 0) {
        for (size_t i = 0; i < len; i += 11) data[i] = 'x';
      } else if (pattern == 2 && len) {
        data[len - 1] = 'x';
      } else if (pattern == 3) {
        for (size_t i = 1; i < len; i += 2) data[i] = 'x';
      } else if (pattern == 4 && len) {
        if (len > 1) data[len - 2] = 'x';
        data[len - 1] = 'y';
      }
      size_t pos = 0, expected = 0, end = 0;
      uint64_t bits = 0;
      do {
        const uint8_t want = SpaceOracle(data, expected, len);
        EXPECT_EQ(rvv::skip_space_safe(data, pos, len, end, bits), want);
        ASSERT_EQ(pos, expected);
      } while (pos < len);
      EXPECT_EQ(rvv::skip_space_safe(data, pos, len, end, bits),
                len ? data[len - 1] : 0);
      EXPECT_EQ(pos, len);
    }
  }
  EXPECT_EQ(munmap(mapping, 2 * page), 0);
}
#endif

struct LazyRecorder {
  using Allocator = SimpleAllocator;
  Allocator allocator;
  std::vector<std::string> values;
  bool StartArray() { return true; }
  bool EndArray(size_t) { return true; }
  bool StartObject() { return true; }
  bool EndObject(size_t) { return true; }
  bool Key(const char* data, size_t, size_t allocated) {
    if (allocated) Allocator::Free(const_cast<char*>(data));
    return true;
  }
  bool Raw(const char* data, size_t len) {
    values.emplace_back(data, len);
    return true;
  }
  Allocator& GetAllocator() { return allocator; }
};

TEST(RvvCache, ReusedParserStartsLazyInputWithFreshClassifications) {
  Parser<> parser;
  std::string input(63, ' ');
  input += "[1]";
  input.reserve(256);
  LazyRecorder first;
  ASSERT_EQ(parser.ParseLazy(reinterpret_cast<const uint8_t*>(input.data()),
                             input.size(), first).Error(), kErrorNone);
  ASSERT_EQ(first.values, std::vector<std::string>({"1"}));

  // The allocation is reused, so a data-pointer identity check is insufficient.
  input = "  [2]" + std::string(100, ' ');
  LazyRecorder second;
  ASSERT_EQ(parser.ParseLazy(reinterpret_cast<const uint8_t*>(input.data()),
                             input.size(), second).Error(), kErrorNone);
  EXPECT_EQ(second.values, std::vector<std::string>({"2"}));
}

TEST(RvvCache, ReusedOnDemandSearchStartsWithFreshClassifications) {
  internal::SkipScanner scanner;
  const JsonPointer path{"target"};
  const std::string first = std::string(63, ' ') + "{\"target\":1}";
  const std::string second = "  {\"target\":2}" + std::string(100, ' ');
  StringView target;
  for (const std::string* input : {&first, &second}) {
    const StringView json(input->data(), input->size());
    size_t pos = 0;
    const long start = scanner.GetOnDemand(json, pos, path);
    ASSERT_GE(start, 0);
    EXPECT_EQ(json.substr(static_cast<size_t>(start), pos - start),
              input == &first ? "1" : "2");
    ASSERT_EQ(GetOnDemand(json, path, target).Error(), kErrorNone);
    EXPECT_EQ(target, input == &first ? "1" : "2");
  }
}

TEST(RvvCache, OnDemandContinuesAfterSkippedStringsAndContainers) {
  for (size_t gap : {size_t{0}, size_t{1}, size_t{3}, size_t{31}, size_t{64}}) {
    const std::string spaces(gap, ' ');
    const std::string input = "{\"s\":\"inside  \\\" text\"," + spaces +
        "\"a\":[1,{\"nested\":true}]," + spaces +
        "\"target\": {\"value\":42},\"tail\":\"" +
        std::string(100, 'z') + "\"}";
    StringView target;
    const auto result = GetOnDemand(StringView(input.data(), input.size()),
                                    JsonPointer{"target", "value"}, target);
    ASSERT_EQ(result.Error(), kErrorNone);
    EXPECT_EQ(target, "42");
  }
}
}  // namespace
#endif
