/*
 * Copyright 2026 ByteDance Inc.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *     http://www.apache.org/licenses/LICENSE-2.0
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
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "sonic/internal/arch/rvv/quote.h"

#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {

using namespace sonic_json;
namespace rvv = sonic_json::internal::rvv;

std::vector<size_t> BoundedStringLengths() {
  std::vector<size_t> lengths;
  for (size_t n = 0; n <= 65; ++n) lengths.push_back(n);
  const size_t vlmax = __riscv_vsetvlmax_e8m4();
  for (size_t n : {vlmax, 2 * vlmax, 3 * vlmax}) {
    lengths.push_back(n - 1);
    lengths.push_back(n);
    lengths.push_back(n + 1);
    lengths.push_back(n + 7);
  }
  std::sort(lengths.begin(), lengths.end());
  lengths.erase(std::unique(lengths.begin(), lengths.end()), lengths.end());
  return lengths;
}

std::vector<uint8_t> PaddedString(const std::string& text, size_t offset) {
  std::vector<uint8_t> bytes(offset + text.size() + 3 + 16, 0xa5);
  std::memcpy(bytes.data() + offset, text.data(), text.size());
  bytes[offset + text.size()] = 'x';
  bytes[offset + text.size() + 1] = '"';
  bytes[offset + text.size() + 2] = 'x';
  return bytes;
}

template <ParseFlags flags>
void CheckBoundedString(const std::string& text, size_t remaining,
                        size_t offset, SonicError initial_error = kErrorNone) {
  ASSERT_LE(remaining, text.size());
  SCOPED_TRACE(::testing::Message()
               << "length " << text.size() << " remaining " << remaining
               << " offset " << offset << " flags " << static_cast<int>(flags)
               << " initial error " << static_cast<int>(initial_error));
  auto expected = PaddedString(text, offset);
  auto actual = expected;
  uint8_t* old_cursor = expected.data() + offset;
  uint8_t* new_cursor = actual.data() + offset;
  SonicError old_error = initial_error;
  SonicError new_error = initial_error;
  const size_t old_size = rvv::parseStringInplace<flags>(old_cursor, old_error);
  const size_t new_size =
      rvv::parseStringInplacePadded<flags>(new_cursor, remaining, new_error);
  EXPECT_EQ(new_size, old_size);
  EXPECT_EQ(new_error, old_error);
  EXPECT_EQ(new_cursor - actual.data(), old_cursor - expected.data());
  EXPECT_TRUE(actual == expected);
}

TEST(RvvBoundedString, PlainPrefixesAndTrailingContentMatchOldDecoder) {
  for (size_t n : BoundedStringLengths()) {
    const std::string text = std::string(n, 'a') + "\"\x01\\q" +
                             std::string(137, 'z');
    for (size_t offset = 0; offset < 8; ++offset) {
      for (size_t remaining : {n + 1, text.size()}) {
        CheckBoundedString<ParseFlags::kParseDefault>(text, remaining, offset);
        CheckBoundedString<ParseFlags::kParseAllowUnescapedControlChars>(
            text, remaining, offset);
      }
    }
    CheckBoundedString<ParseFlags::kParseDefault>(
        text, text.size(), 3, kParseErrorInvalidChar);
  }
  CheckBoundedString<ParseFlags::kParseDefault>("", 0, 0);
  CheckBoundedString<ParseFlags::kParseAllowUnescapedControlChars>("", 0, 7);
}

TEST(RvvBoundedString, EscapesAndErrorsRetainLookaheadPriorityAndMutations) {
  const std::vector<std::string> suffixes = {
      "\\q\x01\"", "\\q\"\x01", "\\q" + std::string(20, 'x') + "\x01\"",
      "\\u12xz\"", "\\uD800x\"", "\\uDC00\"", "\\uD800\\u0041\"",
      "\\uD800\\uDC00tail\"", "\\n\\t\\u0041tail\"",
      "\\n" + std::string(97, 'x') + "\\q\x01\"",
      "\\n" + std::string(97, 'x') + "\\uD800x\"",
      std::string(1, '\0') + "\"", std::string(1, '\x1f') + "\"",
      std::string(31, '\x01') + "\\n\"",
      "\\\"\\\\\\/\\b\\f\\n\\r\\t\\u0000tail\""};
  for (size_t n : BoundedStringLengths()) {
    for (const auto& suffix : suffixes) {
      const std::string text = std::string(n, 'x') + suffix;
      CheckBoundedString<ParseFlags::kParseDefault>(text, text.size(), n % 8);
      CheckBoundedString<ParseFlags::kParseAllowUnescapedControlChars>(
          text, text.size(), n % 8);
    }
  }
}

TEST(RvvBoundedString, DeterministicRandomInputsMatchOldDecoder) {
  std::mt19937_64 random(0x7be1a932);
  const size_t max_length = 3 * __riscv_vsetvlmax_e8m4() + 57;
  const std::vector<std::string> events = {
      "\"", "\\", "\\n", "\\q", "\\u0041", "\\uD800\\uDC00", "\\uD800x",
      "\\u12xz", std::string(1, '\0'), std::string(1, '\x1f'), "\xc3\xa9"};
  for (size_t trial = 0; trial < 2000; ++trial) {
    std::string text(random() % max_length, 'a');
    for (size_t i = 0; i < 3; ++i) {
      const auto& event = events[random() % events.size()];
      text.insert(random() % (text.size() + 1), event);
    }
    text += "\",\"next\":1234567890";
    const size_t offset = random() % 8;
    const SonicError initial_error =
        trial % 17 == 0 ? kParseErrorInvalidChar : kErrorNone;
    SCOPED_TRACE(trial);
    CheckBoundedString<ParseFlags::kParseDefault>(text, text.size(), offset,
                                                 initial_error);
    CheckBoundedString<ParseFlags::kParseAllowUnescapedControlChars>(
        text, text.size(), offset, initial_error);
  }
}

template <ParseFlags flags>
void CheckPublicStringError(const std::string& body) {
  auto expected = PaddedString(body, 0);
  uint8_t* cursor = expected.data();
  SonicError error = kErrorNone;
  rvv::parseStringInplace<flags>(cursor, error);
  const size_t expected_offset =
      1 + static_cast<size_t>(cursor - expected.data());
  const std::string json = '"' + body;
  if (expected_offset > json.size()) error = kParseErrorInvalidChar;
  Document document;
  document.Parse<flags>(json.data(), json.size());
  EXPECT_EQ(document.GetParseError(), error);
  EXPECT_EQ(document.GetErrorOffset(), expected_offset);
}

TEST(RvvBoundedString, PublicParserPreservesErrorOffsetsAndLongStrings) {
  for (size_t n : BoundedStringLengths()) {
    SCOPED_TRACE(n);
    const std::string prefix(n, 'x');
    for (const auto& suffix : {
             std::string("\\q\x01\""), std::string("\\u12xz\""),
             std::string("\\uD800x\""), std::string("\\uDC00\""),
             std::string("\x01\""), std::string()}) {
      CheckPublicStringError<ParseFlags::kParseDefault>(prefix + suffix);
      CheckPublicStringError<ParseFlags::kParseAllowUnescapedControlChars>(
          prefix + suffix);
    }
    const std::string key = prefix + "key";
    const std::string value = prefix + '\n' + "Atail";
    const std::string json =
        "{\"" + key + "\":\"" + prefix + "\\n\\u0041tail\"}";
    Document document;
    document.Parse(json.data(), json.size());
    ASSERT_FALSE(document.HasParseError());
    EXPECT_EQ(document[key.c_str()].GetStringView(),
              StringView(value.data(), value.size()));
    document.ParseOnDemand(json.data(), json.size(), JsonPointer{key.c_str()});
    ASSERT_FALSE(document.HasParseError());
    EXPECT_EQ(document.GetStringView(), StringView(value.data(), value.size()));
  }
}

#if defined(__linux__)
class BoundedStringGuard {
 public:
  explicit BoundedStringGuard(size_t capacity) {
    const long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) return;
    const size_t page_size = static_cast<size_t>(page);
    const size_t readable =
        ((capacity + page_size - 1) / page_size) * page_size;
    mapping_size_ = readable + page_size;
    mapping_ = mmap(nullptr, mapping_size_, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping_ == MAP_FAILED) return;
    auto* base = static_cast<uint8_t*>(mapping_);
    if (mprotect(base + readable, page_size, PROT_NONE) != 0) {
      munmap(mapping_, mapping_size_);
      mapping_ = MAP_FAILED;
      return;
    }
    data_ = base + readable - capacity;
  }
  ~BoundedStringGuard() {
    if (mapping_ != MAP_FAILED) munmap(mapping_, mapping_size_);
  }
  uint8_t* data() const { return data_; }

 private:
  void* mapping_{MAP_FAILED};
  size_t mapping_size_{0};
  uint8_t* data_{nullptr};
};

template <ParseFlags flags>
void CheckBoundedStringGuard(const std::string& text, bool terminated) {
  std::vector<uint8_t> expected(text.size() + (terminated ? 16 : 19), 0xa5);
  std::memcpy(expected.data(), text.data(), text.size());
  if (!terminated) {
    expected[text.size()] = 'x';
    expected[text.size() + 1] = '"';
    expected[text.size() + 2] = 'x';
  }
  BoundedStringGuard guarded(expected.size());
  ASSERT_NE(guarded.data(), nullptr);
  std::memcpy(guarded.data(), expected.data(), expected.size());
  uint8_t* old_cursor = expected.data();
  uint8_t* new_cursor = guarded.data();
  SonicError old_error = kErrorNone;
  SonicError new_error = kErrorNone;
  const size_t old_size = rvv::parseStringInplace<flags>(old_cursor, old_error);
  const size_t new_size =
      rvv::parseStringInplacePadded<flags>(new_cursor, text.size(), new_error);
  EXPECT_EQ(new_size, old_size);
  EXPECT_EQ(new_error, old_error);
  EXPECT_EQ(new_cursor - guarded.data(), old_cursor - expected.data());
  EXPECT_EQ(std::memcmp(guarded.data(), expected.data(), expected.size()), 0);
}

TEST(RvvBoundedString, StopsWithinExistingPaddingAtGuardPage) {
  for (size_t n : BoundedStringLengths()) {
    SCOPED_TRACE(n);
    const std::string prefix(n, 'x');
    for (const auto& text : {prefix + '"', prefix + "\\n\\u0041tail\"",
                             prefix + "\\u12xz\"", prefix + "\x01\""}) {
      CheckBoundedStringGuard<ParseFlags::kParseDefault>(text, true);
      CheckBoundedStringGuard<ParseFlags::kParseAllowUnescapedControlChars>(
          text, true);
    }
    CheckBoundedStringGuard<ParseFlags::kParseDefault>(prefix, false);
    CheckBoundedStringGuard<ParseFlags::kParseAllowUnescapedControlChars>(
        prefix, false);
  }
}
#endif

}  // namespace

#endif  // SONIC_HAVE_RVV
