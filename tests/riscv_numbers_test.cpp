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

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "gtest/gtest.h"
#include "sonic/internal/arch/riscv/itoa.h"
#include "sonic/internal/itoa.h"
#include "sonic/sonic.h"

namespace {

void CheckFixedWidth(uint64_t value, size_t width, size_t offset) {
  SCOPED_TRACE(::testing::Message() << "value=" << value << " width=" << width
                                    << " offset=" << offset);
  alignas(8) std::array<char, 40> actual;
  actual.fill('@');
  auto expected = actual;
  char* output = actual.data() + 8 + offset;
  std::string digits = std::to_string(value);
  ASSERT_LE(digits.size(), width);
  digits.insert(0, width - digits.size(), '0');
  std::memcpy(expected.data() + 8 + offset, digits.data(), width);

  char* end = width == 8 ? sonic_json::internal::riscv::Utoa_8(
                               static_cast<uint32_t>(value), output)
                         : sonic_json::internal::riscv::Utoa_16(value, output);
  EXPECT_EQ(end, output + width);
  EXPECT_EQ(actual, expected);
}

template <typename Integer, typename Format>
void CheckInteger(Integer value, size_t offset, Format format) {
  SCOPED_TRACE(::testing::Message()
               << "value=" << value << " offset=" << offset);
  alignas(8) std::array<char, 56> buffer;
  buffer.fill('@');
  char* output = buffer.data() + 8 + offset;
  char* end = format(output, value);
  const std::string expected = std::to_string(value);
  ASSERT_EQ(end, output + expected.size());
  EXPECT_EQ(std::string(output, end), expected);
  const auto unchanged = [](char byte) { return byte == '@'; };
  EXPECT_TRUE(std::all_of(buffer.data(), output, unchanged));
  // The variable-width APIs permit writes within their 32-byte output buffer.
  EXPECT_TRUE(
      std::all_of(output + 32, buffer.data() + buffer.size(), unchanged));
}

TEST(RiscvNumbers, FixedWidthBoundariesAndAlignment) {
  for (size_t offset = 0; offset < 8; ++offset) {
    uint64_t power = 1;
    for (int digits = 0; digits <= 16; ++digits) {
      for (uint64_t value : {power - 1, power, power + 1}) {
        if (value < 100000000) CheckFixedWidth(value, 8, offset);
        if (value < 10000000000000000ULL) CheckFixedWidth(value, 16, offset);
      }
      if (digits != 16) power *= 10;
    }
  }
}

TEST(RiscvNumbers, IntegerLimitsAndThresholds) {
  for (size_t offset = 0; offset < 8; ++offset) {
    CheckInteger(UINT64_MAX, offset, sonic_json::internal::U64toa);
    CheckInteger(INT64_MAX, offset, sonic_json::internal::I64toa);
    CheckInteger(INT64_MIN, offset, sonic_json::internal::I64toa);
    uint64_t power = 1;
    for (int digits = 0; digits <= 19; ++digits) {
      for (uint64_t value : {power - 1, power, power + 1}) {
        CheckInteger(value, offset, sonic_json::internal::U64toa);
        if (value <= uint64_t(INT64_MAX)) {
          const int64_t signed_value = static_cast<int64_t>(value);
          CheckInteger(signed_value, offset, sonic_json::internal::I64toa);
          CheckInteger(-signed_value, offset, sonic_json::internal::I64toa);
        }
      }
      if (digits != 19) power *= 10;
    }
  }
}

TEST(RiscvNumbers, DeterministicRandomIntegers) {
  uint64_t state = 0x123456789abcdef0ULL;
  for (size_t i = 0; i < 1024; ++i) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    const size_t offset = i % 8;
    CheckFixedWidth(state % 100000000, 8, offset);
    CheckFixedWidth(state % 10000000000000000ULL, 16, offset);
    CheckInteger(state, offset, sonic_json::internal::U64toa);
    int64_t signed_value = static_cast<int64_t>(state >> 1);
    if (i % 2 != 0) signed_value = -signed_value;
    CheckInteger(signed_value, offset, sonic_json::internal::I64toa);
  }
}

TEST(RiscvNumbers, SerializeIntegerBoundaries) {
  const char* json =
      "[0,9,10,99,100,9999,10000,99999999,100000000,9999999999999999,"
      "10000000000000000,18446744073709551615,-9223372036854775808]";
  sonic_json::Document document;
  document.Parse(json);
  ASSERT_FALSE(document.HasParseError());
  EXPECT_EQ(document.Dump(), json);
}

uint64_t DoubleBits(double value) {
  uint64_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

template <ParseFlags flags>
void CheckFloatingToken(const std::string& token) {
  SCOPED_TRACE(::testing::Message() << "token=" << token << " flags="
                                    << static_cast<unsigned>(flags));
  sonic_json::Document document;
  document.Parse<flags>(token.data(), token.size());
  ASSERT_FALSE(document.HasParseError());
  ASSERT_TRUE(document.IsDouble());
  char* end = nullptr;
  const double expected = std::strtod(token.c_str(), &end);
  ASSERT_EQ(end, token.c_str() + token.size());
  EXPECT_EQ(DoubleBits(document.GetDouble()), DoubleBits(expected));
}

void CheckFloatingFlags(const std::string& token) {
  CheckFloatingToken<ParseFlags::kParseDefault>(token);
  CheckFloatingToken<ParseFlags::kParseIntegerAsRaw>(token);
  CheckFloatingToken<ParseFlags::kParseOverflowNumAsNumStr>(token);
  CheckFloatingToken<ParseFlags::kParseIntegerAsRaw |
                     ParseFlags::kParseOverflowNumAsNumStr>(token);
}

template <ParseFlags flags>
void CheckNumberError(const std::string& token, size_t expected_offset) {
  SCOPED_TRACE(::testing::Message() << "token=" << token << " flags="
                                    << static_cast<unsigned>(flags));
  sonic_json::Document document;
  document.Parse<flags>(token.data(), token.size());
  EXPECT_EQ(document.GetParseError(), sonic_json::kParseErrorInvalidChar);
  EXPECT_EQ(document.GetErrorOffset(), expected_offset);
}

void CheckNumberErrorFlags(const std::string& token, size_t expected_offset) {
  CheckNumberError<ParseFlags::kParseDefault>(token, expected_offset);
  CheckNumberError<ParseFlags::kParseIntegerAsRaw>(token, expected_offset);
  CheckNumberError<ParseFlags::kParseOverflowNumAsNumStr>(token,
                                                          expected_offset);
  CheckNumberError<ParseFlags::kParseIntegerAsRaw |
                   ParseFlags::kParseOverflowNumAsNumStr>(token,
                                                          expected_offset);
}

TEST(RiscvNumbers, LongIntegerFloatSuffixesAndFlags) {
  for (size_t length : {20, 21, 32, 64, 128, 256}) {
    std::string digits;
    while (digits.size() < length) digits += "9876543210";
    digits.resize(length);
    for (const char* sign : {"", "-"}) {
      const std::string integer = sign + digits;
      SCOPED_TRACE(integer);
      sonic_json::Document document;
      document.Parse<ParseFlags::kParseIntegerAsRaw>(integer);
      ASSERT_FALSE(document.HasParseError());
      ASSERT_TRUE(document.IsRaw());
      EXPECT_EQ(document.GetRaw(), integer);
      document.Parse<ParseFlags::kParseOverflowNumAsNumStr>(integer);
      ASSERT_FALSE(document.HasParseError());
      ASSERT_TRUE(document.IsStringNumber());
      EXPECT_EQ(document.GetStringView(), integer);
      document.Parse<ParseFlags::kParseIntegerAsRaw |
                     ParseFlags::kParseOverflowNumAsNumStr>(integer);
      ASSERT_FALSE(document.HasParseError());
      ASSERT_TRUE(document.IsRaw());
      EXPECT_EQ(document.GetRaw(), integer);
      CheckFloatingToken<ParseFlags::kParseDefault>(integer);
      for (const char* suffix : {".125", "e-20", ".125e-20"}) {
        CheckFloatingFlags(integer + suffix);
      }
    }
  }
}

TEST(RiscvNumbers, LongIntegerErrorOffsets) {
  for (size_t length : {20, 21, 32, 64, 256, 1024}) {
    const std::string integer = "1" + std::string(length - 1, '2');
    for (const std::string suffix : {".", "e", "e+", "e-", ".e1"}) {
      const size_t expected = length + (suffix == ".e1" ? 1 : suffix.size());
      CheckNumberErrorFlags(integer + suffix, expected);
    }
  }
}

TEST(RiscvNumbers, FractionLengthsFlagsAndRounding) {
  for (size_t length :
       {1, 3, 4, 5, 7, 8, 9, 11, 12, 13, 15, 16, 17, 18, 31, 32, 64, 256}) {
    std::string digits;
    while (digits.size() < length) digits += "1234567890";
    digits.resize(length);
    for (const char* prefix : {"0.", "-0.", "1234567890123456."}) {
      for (const char* suffix : {"", "e-20", "e20", "e-400"}) {
        CheckFloatingFlags(prefix + digits + suffix);
      }
    }
  }
  for (const std::string& token :
       {"0." + std::string(256, '0') + "12345678",
        "-0." + std::string(512, '0') + "1",
        std::string("1.00000000000000011102230246251565404236316680908203124"),
        std::string("1.00000000000000011102230246251565404236316680908203125"),
        std::string("1.00000000000000011102230246251565404236316680908203126"),
        "1.00000000000000011102230246251565404236316680908203125" +
            std::string(256, '0') + "1"}) {
    CheckFloatingFlags(token);
  }
  for (const std::string& token :
       {"0.1234567890123456e400", "-0.1234567890123456e400"}) {
    sonic_json::Document document;
    document.Parse(token);
    EXPECT_EQ(document.GetParseError(), sonic_json::kParseErrorInfinity);
    document.Parse<ParseFlags::kParseIntegerAsRaw>(token);
    EXPECT_EQ(document.GetParseError(), sonic_json::kParseErrorInfinity);
    document.Parse<ParseFlags::kParseOverflowNumAsNumStr>(token);
    ASSERT_FALSE(document.HasParseError());
    ASSERT_TRUE(document.IsStringNumber());
    EXPECT_EQ(document.GetStringView(), token);
    document.Parse<ParseFlags::kParseIntegerAsRaw |
                   ParseFlags::kParseOverflowNumAsNumStr>(token);
    ASSERT_FALSE(document.HasParseError());
    ASSERT_TRUE(document.IsStringNumber());
    EXPECT_EQ(document.GetStringView(), token);
  }
}

TEST(RiscvNumbers, FractionErrorOffsetsAndFlags) {
  for (size_t length : {1, 4, 8, 12, 16, 17, 32}) {
    for (size_t padding = 0; padding < 8; ++padding) {
      const std::string prefix =
          std::string(padding, ' ') + "0." + std::string(length, '2');
      for (const std::string suffix :
           {".", "e", "e+", "e-", "e+x", "x", " x"}) {
        size_t offset = prefix.size();
        if (suffix == "e" || suffix == " x") ++offset;
        if (suffix == "e+" || suffix == "e-" || suffix == "e+x") offset += 2;
        CheckNumberErrorFlags(prefix + suffix, offset);
      }
    }
  }
}

TEST(RiscvNumbers, FractionEveryTerminatorByteAndAlignment) {
  // The first four fraction digits precede the eight-byte conversion block.
  // Trailing spaces keep every inspected word inside the logical input.
  for (size_t padding = 0; padding < 8; ++padding) {
    for (size_t lane = 0; lane < 8; ++lane) {
      const std::string prefix =
          std::string(padding, ' ') + "0.1234" + std::string(lane, '5');
      for (unsigned byte = 0; byte < 256; ++byte) {
        SCOPED_TRACE(::testing::Message() << "padding=" << padding << " lane="
                                          << lane << " byte=" << byte);
        const std::string number = prefix + static_cast<char>(byte) + "1";
        const std::string json = number + std::string(8, ' ');
        sonic_json::Document document;
        document.Parse(json.data(), json.size());
        if ((byte >= '0' && byte <= '9') || byte == 'e' || byte == 'E') {
          ASSERT_FALSE(document.HasParseError());
          ASSERT_TRUE(document.IsDouble());
          char* end = nullptr;
          const double expected = std::strtod(number.c_str(), &end);
          ASSERT_EQ(end, number.c_str() + number.size());
          EXPECT_EQ(DoubleBits(document.GetDouble()), DoubleBits(expected));
        } else {
          EXPECT_EQ(document.GetParseError(),
                    sonic_json::kParseErrorInvalidChar);
          const bool whitespace =
              byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n';
          EXPECT_EQ(document.GetErrorOffset(), prefix.size() + whitespace);
        }
      }
    }
  }
}

}  // namespace
