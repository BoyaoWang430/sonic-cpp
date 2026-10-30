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

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>
#include <string>

#include "sonic/internal/arch/rvv/itoa.h"

#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {

using namespace sonic_json;
namespace rvv = sonic_json::internal::rvv;

std::string FixedDecimal(uint64_t value, size_t width) {
  std::string result = std::to_string(value);
  return std::string(width - result.size(), '0') + result;
}

TEST(RvvNumber, PublicFractionParsing) {
  std::mt19937_64 random(0x5e1f203);
  for (size_t i = 0; i < 4000; ++i) {
    const size_t fraction_length = 1 + random() % 20;
    std::string fraction(fraction_length, '0');
    for (char& ch : fraction) ch = static_cast<char>('0' + random() % 10);
    const std::string number = (i % 2 ? "-0." : "0.") + fraction + "e" +
                               std::to_string(static_cast<int>(i % 21) - 10);
    // Following digits must not be consumed as part of a short fraction.
    const std::string json = "[" + number + ",1234567890123456]";
    Document doc;
    doc.Parse(json.data(), json.size());
    ASSERT_FALSE(doc.HasParseError()) << json;
    ASSERT_TRUE(doc[0].IsDouble());
    EXPECT_DOUBLE_EQ(doc[0].GetDouble(), std::strtod(number.c_str(), nullptr));
  }
}

TEST(RvvNumber, FixedWidthOutputAndPublicIntegerSerialization) {
  char output[32];
  auto check = [&](uint64_t value) {
    std::memset(output, '?', sizeof(output));
    char* const begin = output + 1;
    EXPECT_EQ(rvv::Utoa_16(value, begin), begin + 16);
    EXPECT_EQ(std::string(begin, 16), FixedDecimal(value, 16));
    EXPECT_EQ(output[0], '?');
    EXPECT_EQ(begin[16], '?');
  };
  for (uint64_t group = 0; group < 10000; ++group) {
    for (uint64_t scale : {uint64_t{1}, uint64_t{10000}, uint64_t{100000000},
                           uint64_t{1000000000000}}) {
      check(group * scale);
    }
  }
  std::mt19937_64 random(0xd123456);
  for (size_t i = 0; i < 4000; ++i) {
    const uint64_t value = random();
    check(value % 10000000000000000ull);
    Node node(value);
    EXPECT_EQ(node.Dump(), std::to_string(value));
  }
  for (uint64_t value : {uint64_t{0}, uint64_t{99999999}, uint64_t{100000000},
                         uint64_t{9999999999999999}, uint64_t{10000000000000000},
                         std::numeric_limits<uint64_t>::max()}) {
    Node node(value);
    EXPECT_EQ(node.Dump(), std::to_string(value));
  }
  for (int64_t value : {std::numeric_limits<int64_t>::min(), int64_t{-1},
                        std::numeric_limits<int64_t>::max()}) {
    Node node(value);
    EXPECT_EQ(node.Dump(), std::to_string(value));
  }
}

#if defined(__linux__)
class NumberGuardedPage {
 public:
  NumberGuardedPage() {
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return;
    page_ = static_cast<size_t>(page_size);
    mapping_ = mmap(nullptr, page_ * 3, PROT_NONE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping_ != MAP_FAILED) {
      if (mprotect(static_cast<char*>(mapping_) + page_, page_,
                   PROT_READ | PROT_WRITE) == 0) {
        end_ = static_cast<char*>(mapping_) + page_ * 2;
      }
    }
  }
  ~NumberGuardedPage() {
    if (mapping_ != MAP_FAILED) munmap(mapping_, page_ * 3);
  }
  char* end() const { return end_; }

 private:
  size_t page_ = 0;
  void* mapping_ = MAP_FAILED;
  char* end_ = nullptr;
};

TEST(RvvNumber, ExactOutputAtGuardPage) {
  NumberGuardedPage buffer;
  ASSERT_NE(buffer.end(), nullptr);
  char* const output = buffer.end() - 16;
  constexpr uint64_t value = 9999999999999999ull;
  EXPECT_EQ(rvv::Utoa_16(value, output), buffer.end());
  EXPECT_EQ(std::string(output, 16), FixedDecimal(value, 16));
}

#endif

}  // namespace
#endif
