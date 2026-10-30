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

#include "sonic/internal/arch/rvv/simd.h"

#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {

using namespace sonic_json;
namespace rvv = sonic_json::internal::rvv;

std::vector<size_t> BoundaryLengths() {
  std::vector<size_t> lengths;
  for (size_t n = 0; n <= 65; ++n) lengths.push_back(n);
  const size_t vlmax = __riscv_vsetvlmax_e8m4();
  for (size_t n : {vlmax, 2 * vlmax}) {
    lengths.push_back(n - 1);
    lengths.push_back(n);
    lengths.push_back(n + 1);
  }
  std::sort(lengths.begin(), lengths.end());
  lengths.erase(std::unique(lengths.begin(), lengths.end()), lengths.end());
  return lengths;
}

std::string QuoteOracle(const std::string& input) {
  static const char hex[] = "0123456789abcdef";
  std::string out = "\"";
  for (unsigned char ch : input) {
    switch (ch) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (ch < 0x20) {
          out += "\\u00";
          out += hex[ch >> 4];
          out += hex[ch & 15];
        } else {
          out += static_cast<char>(ch);
        }
    }
  }
  return out + '"';
}

std::string Payload(size_t n) {
  static const char pattern[] = {'a', '"', '\\', '[', '}', '\n', '\0',
                                 '\x1f', 'z', '\t', '/', '\x7f'};
  std::string result(n, 'x');
  for (size_t i = 0; i < n; ++i) result[i] = pattern[i % sizeof(pattern)];
  return result;
}

int SkipStringOracle(const std::string& text, size_t& pos) {
  int status = 1;
  while (pos < text.size()) {
    const char ch = text[pos++];
    if (ch == '"') return status;
    if (ch == '\\') {
      status = 2;
      if (pos == text.size()) return 0;
      ++pos;
    }
  }
  return 0;
}

bool SkipContainerOracle(const std::string& text, size_t& pos) {
  std::vector<char> closes(1, text[pos - 1] == '[' ? ']' : '}');
  while (pos < text.size()) {
    const char ch = text[pos++];
    if (ch == '"') {
      if (!SkipStringOracle(text, pos)) return false;
    } else if (ch == '[' || ch == '{') {
      closes.push_back(ch == '[' ? ']' : '}');
    } else if (ch == ']' || ch == '}') {
      if (closes.empty() || closes.back() != ch) return false;
      closes.pop_back();
      if (closes.empty()) return true;
    }
  }
  return false;
}

uint32_t NextRandom(uint32_t& state) {
  state = state * 1664525u + 1013904223u;
  return state;
}

std::string RandomValue(uint32_t& state, unsigned depth) {
  const unsigned kind = NextRandom(state) % (depth ? 5 : 3);
  if (kind == 0) return QuoteOracle(Payload(NextRandom(state) % 150));
  if (kind == 1) return std::to_string(NextRandom(state));
  if (kind == 2) return "true";
  const bool object = kind == 3;
  std::string out = object ? "{" : "[";
  const size_t count = 1 + NextRandom(state) % 3;
  for (size_t i = 0; i < count; ++i) {
    if (i) out += ", \n";
    if (object) out += QuoteOracle("key" + std::to_string(i)) + ":";
    out += RandomValue(state, depth - 1);
  }
  return out + (object ? '}' : ']');
}

TEST(Rvv, VectorLengthAndMaskBits) {
  const size_t vlmax = __riscv_vsetvlmax_e8m4();
  EXPECT_EQ(rvv::SetVL(0), 0u);
  for (size_t requested : BoundaryLengths()) {
    const size_t vl = rvv::SetVL(requested);
    EXPECT_LE(vl, requested);
    EXPECT_LE(vl, vlmax);
    if (requested) {
      EXPECT_GT(vl, 0u);
    }
  }

  uint8_t bytes[64];
  for (unsigned pattern = 0; pattern != 3; ++pattern) {
    for (size_t i = 0; i < 64; ++i) {
      bytes[i] = pattern == 0 ? 1 : pattern == 1 ? 0 : (i % 3 == 0);
    }
    const size_t vl = rvv::SetVL(64);
    ASSERT_GT(vl, 0u);
    ASSERT_LE(vl, 64u);
    const auto data = __riscv_vle8_v_u8m4(bytes, vl);
    const auto mask = __riscv_vmseq_vx_u8m4_b2(data, 1, vl);
    uint64_t expected = 0;
    for (size_t n = 0; n <= vl; ++n) {
      if (n && bytes[n - 1]) expected |= uint64_t{1} << (n - 1);
      EXPECT_EQ(rvv::MaskBits(mask, n), expected) << "length " << n;
    }
  }
}

TEST(Rvv, NonSpaceMaskClassifiesEveryByte) {
  uint8_t bytes[64];
  for (unsigned first = 0; first < 256; first += 64) {
    for (size_t i = 0; i < 64; ++i) bytes[i] = first + i;
    for (size_t requested = 1; requested <= 64; ++requested) {
      const size_t vl = rvv::SetVL(requested);
      const auto data = __riscv_vle8_v_u8m4(bytes, vl);
      uint64_t expected = 0;
      for (size_t i = 0; i < vl; ++i) {
        const uint8_t c = bytes[i];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
          expected |= uint64_t{1} << i;
        }
      }
      EXPECT_EQ(rvv::MaskBits(rvv::NonSpaceMask(data, vl), vl), expected);
    }
  }
}

TEST(Rvv, ParseSerializeAtVectorBoundaries) {
  for (size_t n : BoundaryLengths()) {
    for (size_t offset = 0; offset < 8; ++offset) {
      SCOPED_TRACE(::testing::Message()
                   << "length " << n << " offset " << offset);
      const std::string payload = Payload(n);
      const std::string json = QuoteOracle(payload);
      const std::string input = std::string(offset, '!') + json;
      Document doc;
      doc.Parse(input.data() + offset, json.size());
      ASSERT_FALSE(doc.HasParseError()) << doc.GetParseError();
      ASSERT_TRUE(doc.IsString());
      EXPECT_EQ(doc.GetString(), payload);
      EXPECT_EQ(doc.Dump(), json);
    }
  }
}

TEST(Rvv, ParseAndSerializeFlags) {
  for (size_t n : BoundaryLengths()) {
    const std::string prefix(n, 'x');
    const std::string input = '"' + prefix + '\x01' + "\n\"";
    Document doc;
    doc.Parse(input.data(), input.size());
    EXPECT_EQ(doc.GetParseError(), kParseErrorUnEscaped);
    doc.Parse<ParseFlags::kParseAllowUnescapedControlChars>(input.data(),
                                                         input.size());
    ASSERT_FALSE(doc.HasParseError());
    EXPECT_EQ(doc.GetString(), prefix + '\x01' + '\n');
    EXPECT_EQ(doc.Dump(), '"' + prefix + "\\u0001\\n\"");
  }

  Document doc;
  doc.Parse("\"\\ud83d\\ude00\\u001f\"");
  ASSERT_FALSE(doc.HasParseError());
  EXPECT_EQ(doc.Dump<SerializeFlags::kSerializeEscapeEmoji>(),
            "\"\\ud83d\\ude00\\u001f\"");
  constexpr auto flags = SerializeFlags::kSerializeEscapeEmoji |
                         SerializeFlags::kSerializeUnicodeEscapeUppercase;
  EXPECT_EQ(doc.Dump<flags>(), "\"\\uD83D\\uDE00\\u001F\"");
}

template <ParseFlags flags>
void CheckDecodedString(const std::string& encoded,
                        const std::string& expected) {
  std::vector<uint8_t> input(encoded.size() + 1 + 16, '\x01');
  std::memcpy(input.data(), encoded.data(), encoded.size());
  input[encoded.size()] = '"';
  uint8_t* cursor = input.data();
  SonicError error = kErrorNone;
  const size_t written = rvv::parseStringInplace<flags>(cursor, error);
  ASSERT_EQ(error, kErrorNone);
  EXPECT_EQ(written, expected.size());
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(input.data()), written),
            expected);
  EXPECT_EQ(input[written], '\0');
  EXPECT_EQ(cursor, input.data() + encoded.size() + 1);
}

TEST(Rvv, DecodeMultipleEventsAcrossSixteenByteBlocks) {
  struct Case {
    std::string encoded;
    std::string decoded;
  };
  const std::string emoji = "\xf0\x9f\x98\x80";
  const std::vector<Case> cases = {
      {R"(a\nb\tc\"d\\e\/f\b\r\f)", "a\nb\tc\"d\\e/f\b\r\f"},
      {R"(\u0041b\u0000c\u4e2dd\ud83d\ude00)",
       std::string("Ab\0c", 4) + "\xe4\xb8\xad" + "d" + emoji},
      {R"(\"\\\/\b\f\n\r\t)", "\"\\/\b\f\n\r\t"},
      {R"(\uD83D\uDE00\u0041)", emoji + "A"},
      {"\xe4\xb8\xad" + emoji, "\xe4\xb8\xad" + emoji},
  };
  for (size_t prefix_size = 0; prefix_size < 32; ++prefix_size) {
    const std::string prefix(prefix_size, 'x');
    for (const auto& test : cases) {
      SCOPED_TRACE(::testing::Message()
                   << "prefix " << prefix_size << " input " << test.encoded);
      const std::string encoded = prefix + test.encoded + std::string(33, 'z');
      const std::string expected = prefix + test.decoded + std::string(33, 'z');
      CheckDecodedString<ParseFlags::kParseDefault>(encoded, expected);
      CheckDecodedString<ParseFlags::kParseAllowUnescapedControlChars>(encoded,
                                                                    expected);
    }
  }
}

TEST(Rvv, DecodeControlsBetweenEscapesRespectsFlags) {
  for (size_t prefix_size = 0; prefix_size < 32; ++prefix_size) {
    for (char control : {'\0', '\x01', '\x1f'}) {
      SCOPED_TRACE(::testing::Message()
                   << "prefix " << prefix_size << " control " << int(control));
      const std::string prefix(prefix_size, 'x');
      const std::string encoded = prefix + "\\n" + control + "\\t\\u0041tail";
      const std::string expected = prefix + '\n' + control + "\tAtail";
      std::vector<uint8_t> input(encoded.size() + 1 + 16, '\x01');
      std::memcpy(input.data(), encoded.data(), encoded.size());
      input[encoded.size()] = '"';
      uint8_t* cursor = input.data();
      SonicError error = kErrorNone;
      rvv::parseStringInplace<ParseFlags::kParseDefault>(cursor, error);
      EXPECT_EQ(error, kParseErrorUnEscaped);
      CheckDecodedString<ParseFlags::kParseAllowUnescapedControlChars>(encoded,
                                                                    expected);
    }
  }
}

TEST(Rvv, DecodeErrorsRetainLookaheadPriority) {
  const size_t vl = rvv::SetVL(16);
  ASSERT_GT(vl, 0u);
  for (size_t prefix = 0; prefix < vl; ++prefix) {
    for (bool control_before_quote : {false, true}) {
      SCOPED_TRACE(::testing::Message()
                   << "vl " << vl << " prefix " << prefix
                   << " control before quote " << control_before_quote);
      const std::string suffix =
          control_before_quote ? std::string("\\q\x01\"") : "\\q\"\x01";
      const std::string encoded = std::string(prefix, 'x') + suffix;
      std::vector<uint8_t> input(encoded.size() + 16, 'x');
      std::memcpy(input.data(), encoded.data(), encoded.size());
      uint8_t* cursor = input.data();
      SonicError error = kErrorNone;
      rvv::parseStringInplace<ParseFlags::kParseDefault>(cursor, error);
      // A short-VL build may decode the invalid escape before seeing the control.
      const bool control_in_lookahead = prefix + 2 < vl;
      EXPECT_EQ(error, control_before_quote && control_in_lookahead
                           ? kParseErrorUnEscaped
                           : kParseErrorEscapedFormat);

      std::memcpy(input.data(), encoded.data(), encoded.size());
      cursor = input.data();
      error = kErrorNone;
      rvv::parseStringInplace<ParseFlags::kParseAllowUnescapedControlChars>(
          cursor, error);
      EXPECT_EQ(error, kParseErrorEscapedFormat);
    }
  }
}

TEST(Rvv, SkipStringsAndTokensAtBoundaries) {
  for (size_t n : BoundaryLengths()) {
    for (const std::string& payload : {std::string(n, 'x'), Payload(n),
                                      std::string(n, '\\')}) {
      const std::string wire = QuoteOracle(payload);
      for (size_t offset = 0; offset < 8; ++offset) {
        std::string input(offset, '!');
        input += wire;
        size_t expected_pos = offset + 1;
        const int expected = SkipStringOracle(input, expected_pos);
        size_t pos = offset + 1;
        EXPECT_EQ(rvv::SkipString(
                      reinterpret_cast<const uint8_t*>(input.data()), pos,
                      input.size()), expected);
        EXPECT_EQ(pos, expected_pos);
        const size_t wire_end = input.size();
        input += "\\\"\\";
        pos = offset + 1;
        EXPECT_EQ(rvv::SkipString(
                      reinterpret_cast<const uint8_t*>(input.data()), pos,
                      input.size()), expected);
        EXPECT_EQ(pos, expected_pos);
        input.resize(wire_end);
        input.pop_back();
        pos = offset + 1;
        EXPECT_EQ(rvv::SkipString(
                      reinterpret_cast<const uint8_t*>(input.data()), pos,
                      input.size()), 0);
      }
    }

    const std::string input = std::string(n, 'x') + ",]";
    size_t pos = 0;
    EXPECT_EQ(rvv::GetNextToken(reinterpret_cast<const uint8_t*>(input.data()),
                               pos, input.size(), "]},"), ',');
    EXPECT_EQ(pos, n);
    pos = 0;
    EXPECT_EQ(rvv::GetNextToken(reinterpret_cast<const uint8_t*>(input.data()),
                               pos, n, "]},"), 0);
    EXPECT_EQ(pos, n);
  }
}

TEST(Rvv, EmptyAndSingleTokenSets) {
  for (size_t n : BoundaryLengths()) {
    const std::string input = std::string(n, 'x') + ']';
    const auto* data = reinterpret_cast<const uint8_t*>(input.data());
    for (size_t start : {size_t{0}, input.size(), input.size() + 1}) {
      size_t pos = start;
      EXPECT_EQ(rvv::GetNextToken(data, pos, input.size(), ""), 0);
      EXPECT_EQ(pos, std::max(start, input.size()));
    }
    size_t pos = 0;
    EXPECT_EQ(rvv::GetNextToken(data, pos, input.size(), "]"), ']');
    EXPECT_EQ(pos, n);
    pos = 0;
    EXPECT_EQ(rvv::GetNextToken(data, pos, n, "]"), 0);
    EXPECT_EQ(pos, n);
  }
}

TEST(Rvv, UnterminatedStringPreservesTrailingBackslashPosition) {
  for (size_t n : BoundaryLengths()) {
    for (size_t slashes = 1; slashes <= 5; ++slashes) {
      for (size_t offset = 0; offset < 8; ++offset) {
        const std::string input = std::string(offset, '!') + '"' +
                                  std::string(n, 'x') +
                                  std::string(slashes, '\\');
        size_t pos = offset + 1;
        EXPECT_EQ(rvv::SkipString(
                      reinterpret_cast<const uint8_t*>(input.data()), pos,
                      input.size()), 0);
        EXPECT_EQ(pos, input.size() - slashes % 2)
            << "length " << n << " backslashes " << slashes;
      }
    }
  }
}

TEST(Rvv, IncompleteContainerRetainsLegacyBlockPosition) {
  for (size_t n : BoundaryLengths()) {
    for (char opening : {'[', '{'}) {
      for (size_t offset = 0; offset < 8; ++offset) {
        const std::string input = std::string(offset, '!') + opening +
                                  std::string(n, ' ');
        const size_t start = offset + 1;
        size_t pos = start;
        EXPECT_FALSE(rvv::SkipContainer(
            reinterpret_cast<const uint8_t*>(input.data()), pos, input.size(),
            opening, opening == '[' ? ']' : '}'));
        EXPECT_EQ(pos, start + (n / 64) * 64);
      }
    }
  }
}

TEST(Rvv, SkipContainersMatchesIndependentScanner) {
  uint32_t state = 0x72697676;
  std::vector<std::string> inputs;
  for (unsigned i = 0; i < 64; ++i) {
    inputs.push_back("[" + RandomValue(state, 4) + ",{\"last\":42}]");
  }
  // Put even and odd encoded backslash runs across the 64-byte scan boundary.
  for (size_t n : BoundaryLengths()) {
    for (size_t slashes = 1; slashes <= 5; ++slashes) {
      const std::string payload = std::string(n, 'x') +
                                  std::string(slashes, '\\') + "\"}]tail";
      inputs.push_back("[" + QuoteOracle(payload) + ",[]]");
      inputs.push_back("{\"a\":" + QuoteOracle(payload) + ",\"b\":{}}");
    }
  }
  for (const std::string& json : inputs) {
    for (size_t offset = 0; offset < 8; ++offset) {
      const std::string input = std::string(offset, '!') + json + "suffix";
      size_t expected_pos = offset + 1;
      ASSERT_TRUE(SkipContainerOracle(input, expected_pos));
      size_t pos = offset + 1;
      EXPECT_TRUE(rvv::SkipContainer(
          reinterpret_cast<const uint8_t*>(input.data()), pos, input.size(),
          json.front(), json.back()));
      EXPECT_EQ(pos, expected_pos);
      pos = offset + 1;
      EXPECT_FALSE(rvv::SkipContainer(
          reinterpret_cast<const uint8_t*>(input.data()), pos,
          offset + json.size() - 1, json.front(), json.back()));
      EXPECT_EQ(pos, offset + 1 + ((json.size() - 2) / 64) * 64);
    }
  }
}

TEST(Rvv, OnDemandAcrossEscapesAndWhitespace) {
  for (size_t n : BoundaryLengths()) {
    const std::string spaces(n, ' ');
    const std::string json = "{" + spaces + "\"skip\":[" +
                             QuoteOracle(Payload(n)) +
                             ", {\"nested\":[true,null]}],\n\t\"target\":" +
                             spaces + "42}";
    StringView target;
    const auto result = GetOnDemand(StringView(json.data(), json.size()),
                                    JsonPointer{"target"}, target);
    ASSERT_EQ(result.Error(), kErrorNone);
    EXPECT_EQ(std::string(target.data(), target.size()), "42");
    Document doc;
    doc.ParseOnDemand(json.data(), json.size(), JsonPointer{"target"});
    ASSERT_FALSE(doc.HasParseError());
    EXPECT_EQ(doc.GetUint64(), 42u);
  }
}

TEST(Rvv, OnDemandMatchesEscapedKeysAfterPlainBlocks) {
  const size_t vl = rvv::SetVL(64);
  for (size_t n : BoundaryLengths()) {
    for (const std::string& key :
         {"\n" + std::string(n + 65, 'x'),
          std::string(vl - 1, 'x') + '"' + std::string(n + 65, 'y')}) {
      SCOPED_TRACE(n);
      const std::string json = "{" + QuoteOracle(key) +
                               ":42,\"other\":\"\\\\\"}";
      const JsonPointer path{StringView(key.data(), key.size())};
      StringView target;
      const auto result =
          GetOnDemand(StringView(json.data(), json.size()), path, target);
      ASSERT_EQ(result.Error(), kErrorNone);
      EXPECT_EQ(target, "42");
      Document doc;
      doc.ParseOnDemand(json.data(), json.size(), path);
      ASSERT_FALSE(doc.HasParseError());
      EXPECT_EQ(doc.GetUint64(), 42u);
    }
  }
}

#if defined(__linux__)

class GuardedBuffer {
 public:
  explicit GuardedBuffer(size_t size) {
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return;
    const size_t page = static_cast<size_t>(page_size);
    const size_t writable = std::max(page, ((size + page - 1) / page) * page);
    mapped_size_ = writable + 2 * page;
    mapping_ = mmap(nullptr, mapped_size_, PROT_NONE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping_ == MAP_FAILED) return;
    char* begin = static_cast<char*>(mapping_) + page;
    if (mprotect(begin, writable, PROT_READ | PROT_WRITE) != 0) return;
    data_ = begin + writable - size;
  }
  ~GuardedBuffer() {
    if (mapping_ != MAP_FAILED) munmap(mapping_, mapped_size_);
  }
  GuardedBuffer(const GuardedBuffer&) = delete;
  GuardedBuffer& operator=(const GuardedBuffer&) = delete;
  char* data() const { return data_; }

 private:
  void* mapping_ = MAP_FAILED;
  size_t mapped_size_ = 0;
  char* data_ = nullptr;
};

TEST(Rvv, QuoteDoesNotCrossInputOrOutputGuardPage) {
  for (size_t n : BoundaryLengths()) {
    for (bool escaped : {false, true}) {
      const std::string payload = escaped ? Payload(n) : std::string(n, 'x');
      const std::string expected = QuoteOracle(payload);
      GuardedBuffer input(n);
      ASSERT_NE(input.data(), nullptr);
      if (n) std::memcpy(input.data(), payload.data(), n);
      const size_t capacity = escaped ? 6 * n + 35 : expected.size();
      GuardedBuffer output(capacity);
      ASSERT_NE(output.data(), nullptr);
      char* end = rvv::Quote<SerializeFlags::kSerializeDefault>(
          input.data(), n, output.data());
      ASSERT_EQ(end - output.data(), static_cast<ptrdiff_t>(expected.size()));
      EXPECT_EQ(std::string(output.data(), expected.size()), expected);
    }
  }
}

template <SerializeFlags flags>
void CheckGuardedQuote(const std::string& payload,
                       const std::string& expected) {
  GuardedBuffer input(payload.size());
  ASSERT_NE(input.data(), nullptr);
  if (!payload.empty()) {
    std::memcpy(input.data(), payload.data(), payload.size());
  }
  GuardedBuffer output(expected.size());
  ASSERT_NE(output.data(), nullptr);
  char* end = rvv::Quote<flags>(input.data(), payload.size(), output.data());
  ASSERT_EQ(end - output.data(), static_cast<ptrdiff_t>(expected.size()));
  EXPECT_EQ(std::string(output.data(), expected.size()), expected);
}

TEST(Rvv, EmojiFlagsAcrossVectorBoundariesAndGuardPages) {
  const size_t vlmax = __riscv_vsetvlmax_e8m4();
  std::vector<size_t> prefixes = {0, 1, 15, 16, 17};
  for (size_t boundary : {vlmax, 2 * vlmax}) {
    for (size_t n = boundary - 4; n <= boundary + 4; ++n) {
      prefixes.push_back(n);
    }
  }
  const std::string emoji = "\xf0\x9f\x98\x80";
  constexpr auto uppercase = SerializeFlags::kSerializeUnicodeEscapeUppercase;
  constexpr auto escaped = SerializeFlags::kSerializeEscapeEmoji;
  for (size_t n : prefixes) {
    SCOPED_TRACE(n);
    const std::string prefix(n, 'x');
    for (const std::string& suffix :
         {std::string(), std::string(vlmax + 1, 'z')}) {
      const std::string payload = prefix + emoji + suffix + '\x1f' + emoji;
      CheckGuardedQuote<SerializeFlags::kSerializeDefault>(payload,
                                                         QuoteOracle(payload));
      CheckGuardedQuote<uppercase>(
          payload, '"' + prefix + emoji + suffix + "\\u001F" + emoji + '"');
      CheckGuardedQuote<escaped>(payload,
                                '"' + prefix + "\\ud83d\\ude00" + suffix +
                                    "\\u001f\\ud83d\\ude00\"");
      CheckGuardedQuote<escaped | uppercase>(
          payload, '"' + prefix + "\\uD83D\\uDE00" + suffix +
                       "\\u001F\\uD83D\\uDE00\"");
    }
    // The existing escape helper preserves a final one-to-three-byte tail.
    for (size_t bytes = 1; bytes < emoji.size(); ++bytes) {
      const std::string payload = prefix + emoji.substr(0, bytes);
      CheckGuardedQuote<escaped>(payload, QuoteOracle(payload));
      CheckGuardedQuote<escaped | uppercase>(payload, QuoteOracle(payload));
    }
  }
}

TEST(Rvv, SkipSpaceWithOnlySixteenBytesOfLookahead) {
  for (size_t n : BoundaryLengths()) {
    for (uint8_t stop :
         {uint8_t{'x'}, uint8_t{0}, uint8_t{'"'}, uint8_t{0xff}}) {
      GuardedBuffer input(n + 1 + 16);
      ASSERT_NE(input.data(), nullptr);
      for (size_t i = 0; i < n; ++i) input.data()[i] = " \t\r\n"[i % 4];
      input.data()[n] = static_cast<char>(stop);
      std::memset(input.data() + n + 1, '\x01', 16);
      size_t pos = 0, bits_end = 0;
      uint64_t bits = 0;
      EXPECT_EQ(rvv::skip_space(
                    reinterpret_cast<const uint8_t*>(input.data()), pos,
                    bits_end, bits), stop);
      EXPECT_EQ(pos, n + 1);
    }
  }
}

TEST(Rvv, EmptyBoundedScansDoNotReadGuardPage) {
  GuardedBuffer input(0);
  ASSERT_NE(input.data(), nullptr);
  const auto* data = reinterpret_cast<const uint8_t*>(input.data());
  size_t pos = 0, bits_end = 0;
  uint64_t bits = 0;
  EXPECT_EQ(rvv::skip_space_safe(data, pos, 0, bits_end, bits), 0);
  EXPECT_EQ(pos, 0u);
  EXPECT_EQ(rvv::SkipString(data, pos, 0), 0);
  EXPECT_EQ(pos, 0u);
  EXPECT_FALSE(rvv::SkipContainer(data, pos, 0, '[', ']'));
  EXPECT_EQ(pos, 0u);
  EXPECT_EQ(rvv::GetNextToken(data, pos, 0, ""), 0);
  EXPECT_EQ(pos, 0u);
  EXPECT_EQ(rvv::GetNextToken(data, pos, 0, "]"), 0);
  EXPECT_EQ(pos, 0u);
}

TEST(Rvv, InplaceParsingWithOnlySixteenBytesOfLookahead) {
  for (size_t n : BoundaryLengths()) {
    const std::string payload = Payload(n);
    const std::string encoded = QuoteOracle(payload).substr(1);
    GuardedBuffer input(encoded.size() + 16);
    ASSERT_NE(input.data(), nullptr);
    std::memcpy(input.data(), encoded.data(), encoded.size());
    std::memset(input.data() + encoded.size(), 'x', 16);
    input.data()[encoded.size() + 1] = '"';
    uint8_t* cursor = reinterpret_cast<uint8_t*>(input.data());
    SonicError error = kErrorNone;
    const size_t size = rvv::parseStringInplace<ParseFlags::kParseDefault>(
        cursor, error);
    ASSERT_EQ(error, kErrorNone);
    EXPECT_EQ(size, payload.size());
    EXPECT_EQ(std::string(input.data(), size), payload);
    EXPECT_EQ(input.data()[size], '\0');
    EXPECT_EQ(cursor,
              reinterpret_cast<uint8_t*>(input.data() + encoded.size()));
  }
}

TEST(Rvv, InplaceParsingStopsAtPaddingSentinel) {
  for (size_t n : BoundaryLengths()) {
    GuardedBuffer input(n + 3 + 16);
    ASSERT_NE(input.data(), nullptr);
    std::memset(input.data(), 'x', n + 3 + 16);
    input.data()[n + 1] = '"';
    uint8_t* cursor = reinterpret_cast<uint8_t*>(input.data());
    SonicError error = kErrorNone;
    const size_t size = rvv::parseStringInplace<ParseFlags::kParseDefault>(
        cursor, error);
    EXPECT_EQ(error, kErrorNone);
    EXPECT_EQ(size, n + 1);
    EXPECT_EQ(cursor, reinterpret_cast<uint8_t*>(input.data() + n + 2));
  }
}

TEST(Rvv, BoundedScannersStopAtGuardPage) {
  for (size_t n : BoundaryLengths()) {
    for (const std::string& input : {
             QuoteOracle(Payload(n)),
             "[" + QuoteOracle(Payload(n)) + ",{\"x\":[]}]"}) {
      GuardedBuffer buffer(input.size());
      ASSERT_NE(buffer.data(), nullptr);
      std::memcpy(buffer.data(), input.data(), input.size());
      const auto* data = reinterpret_cast<const uint8_t*>(buffer.data());
      size_t pos = 1;
      if (input.front() == '"') {
        EXPECT_NE(rvv::SkipString(data, pos, input.size()), 0);
      } else {
        EXPECT_TRUE(rvv::SkipContainer(data, pos, input.size(), '[', ']'));
      }
      EXPECT_EQ(pos, input.size());
    }
    const std::string spaces(n, ' ');
    GuardedBuffer buffer(n);
    ASSERT_NE(buffer.data(), nullptr);
    if (n) std::memcpy(buffer.data(), spaces.data(), n);
    size_t pos = 0, bits_end = 0;
    uint64_t bits = 0;
    EXPECT_EQ(rvv::skip_space_safe(
                  reinterpret_cast<const uint8_t*>(buffer.data()), pos, n,
                  bits_end, bits), n ? ' ' : 0);
    EXPECT_EQ(pos, n);
    pos = 0;
    EXPECT_EQ(rvv::GetNextToken(
                  reinterpret_cast<const uint8_t*>(buffer.data()), pos, n,
                  "]},"), 0);
    EXPECT_EQ(pos, n);
  }
}

TEST(Rvv, OnDemandReadsUnpaddedGuardedInput) {
  for (size_t n : BoundaryLengths()) {
    const std::string input = "{\"skip\":[" + QuoteOracle(Payload(n)) +
                              ",{}],\"target\":42}";
    GuardedBuffer buffer(input.size());
    ASSERT_NE(buffer.data(), nullptr);
    std::memcpy(buffer.data(), input.data(), input.size());
    StringView target;
    const auto result = GetOnDemand(StringView(buffer.data(), input.size()),
                                    JsonPointer{"target"}, target);
    ASSERT_EQ(result.Error(), kErrorNone);
    EXPECT_EQ(std::string(target.data(), target.size()), "42");
  }
}

#endif  // defined(__linux__)

}  // namespace

#endif  // defined(SONIC_HAVE_RVV)
