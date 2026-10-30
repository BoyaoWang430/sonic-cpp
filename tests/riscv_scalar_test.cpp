/*
 * Copyright 2022 ByteDance Inc.
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
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "gtest/gtest.h"
#include "sonic/internal/arch/riscv/quote.h"
#include "sonic/internal/arch/riscv/skip.h"
#include "sonic/sonic.h"

namespace {

using namespace sonic_json;
namespace scalar = sonic_json::internal::riscv;

const size_t kBoundaries[] = {0,  1,  6,  7,  8,  9,  14, 15,
                              16, 17, 31, 32, 63, 64, 65};

bool IsJsonSpace(uint8_t byte) {
  return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
}

std::string QuoteReference(const std::string& input) {
  const char hex[] = "0123456789abcdef";
  std::string result = "\"";
  for (uint8_t byte : input) {
    switch (byte) {
      case '"':
        result += "\\\"";
        break;
      case '\\':
        result += "\\\\";
        break;
      case '\b':
        result += "\\b";
        break;
      case '\f':
        result += "\\f";
        break;
      case '\n':
        result += "\\n";
        break;
      case '\r':
        result += "\\r";
        break;
      case '\t':
        result += "\\t";
        break;
      default:
        if (byte < 0x20) {
          result += "\\u00";
          result += hex[byte >> 4];
          result += hex[byte & 15];
        } else {
          result += static_cast<char>(byte);
        }
    }
  }
  result += '"';
  return result;
}

TEST(RiscvScalar, BooleanMaskPreservesNibbleFormat) {
  uint8_t bytes[16];
  for (uint32_t bits = 0; bits < 65536; ++bits) {
    uint64_t expected = 0;
    for (unsigned i = 0; i < 16; ++i) {
      const bool set = bits & (uint32_t{1} << i);
      bytes[i] = set ? 0xff : 0;
      if (set) expected |= uint64_t{0xf} << (i * 4);
    }
    EXPECT_EQ(scalar::simd8<bool>(bytes).to_bitmask(), expected)
        << "bits=" << bits;
  }
  for (unsigned byte = 0; byte < 256; ++byte) {
    uint64_t expected = 0;
    for (unsigned i = 0; i < 16; ++i) {
      bytes[i] = i % 3 == 0 ? static_cast<uint8_t>(byte) : 0;
      if (bytes[i]) expected |= uint64_t{0xf} << (i * 4);
    }
    EXPECT_EQ(scalar::simd8<bool>(bytes).to_bitmask(), expected)
        << "byte=" << byte;
  }
}

TEST(RiscvScalar, ExactEqualityMasksAcrossEveryByteAndAlignment) {
  alignas(16) uint8_t storage[80];
  for (size_t alignment = 0; alignment < 16; ++alignment) {
    uint8_t* input = storage + alignment;
    for (unsigned byte = 0; byte < 256; ++byte) {
      uint64_t expected = 0;
      for (unsigned i = 0; i < 64; ++i) {
        const bool match = (i + alignment) % 3 == 0;
        // XOR 1 after a matching byte exposes borrowed bits in zero tests.
        input[i] = static_cast<uint8_t>(match ? byte : byte ^ 1);
        if (match) expected |= uint64_t{1} << i;
      }
      scalar::simd8x64<uint8_t> block(input);
      EXPECT_EQ(block.eq(static_cast<uint8_t>(byte)), expected)
          << "alignment=" << alignment << " byte=" << byte;
    }
  }
}

TEST(RiscvScalar, StringAndWhitespaceClassification) {
  alignas(16) uint8_t storage[32];
  for (size_t alignment = 0; alignment < 16; ++alignment) {
    uint8_t* input = storage + alignment;
    for (unsigned byte = 0; byte < 256; ++byte) {
      for (size_t i = 0; i < 16; ++i) {
        input[i] = static_cast<uint8_t>(i % 2 == 0 ? byte : byte ^ 1);
      }
      unsigned quote = 16, slash = 16, control = 16;
      uint64_t nonspace = 0;
      for (unsigned i = 0; i < 16; ++i) {
        if (input[i] == '"' && quote == 16) quote = i;
        if (input[i] == '\\' && slash == 16) slash = i;
        if (input[i] < 0x20 && control == 16) control = i;
        if (!IsJsonSpace(input[i])) nonspace |= uint64_t{1} << i;
      }
      const auto block = scalar::StringBlock::Find(input);
      EXPECT_EQ(block.HasQuoteFirst<ParseFlags::kParseDefault>(),
                quote < slash && quote < control);
      EXPECT_EQ(
          block.HasQuoteFirst<ParseFlags::kParseAllowUnescapedControlChars>(),
          quote < slash);
      EXPECT_EQ(block.HasBackslash(), slash < quote);
      EXPECT_EQ(block.HasUnescaped(), control < quote);
      if (quote < 16) {
        EXPECT_EQ(block.QuoteIndex(), static_cast<int>(quote));
      }
      if (slash < 16) {
        EXPECT_EQ(block.BsIndex(), static_cast<int>(slash));
      }
      EXPECT_EQ(scalar::GetNonSpaceBits(input), nonspace)
          << "alignment=" << alignment << " byte=" << byte;
    }
  }
}

TEST(RiscvScalar, StringBlockMixedEvents) {
  for (unsigned quote = 0; quote <= 16; ++quote) {
    for (unsigned slash = 0; slash <= 16; ++slash) {
      for (unsigned control = 0; control <= 16; ++control) {
        if ((quote < 16 && (quote == slash || quote == control)) ||
            (slash < 16 && slash == control)) {
          continue;
        }
        SCOPED_TRACE(::testing::Message() << "quote=" << quote << " slash="
                                          << slash << " control=" << control);
        uint8_t input[16];
        std::fill(input, input + 16, 'a');
        if (quote < 16) input[quote] = '"';
        if (slash < 16) input[slash] = '\\';
        if (control < 16) input[control] = 0x1f;
        const auto block = scalar::StringBlock::Find(input);
        EXPECT_EQ(block.HasQuoteFirst<ParseFlags::kParseDefault>(),
                  quote < slash && quote < control);
        EXPECT_EQ(
            block.HasQuoteFirst<ParseFlags::kParseAllowUnescapedControlChars>(),
            quote < slash);
        EXPECT_EQ(block.HasBackslash(), slash < quote);
        EXPECT_EQ(block.HasUnescaped(), control < quote);
        if (quote < 16) EXPECT_EQ(block.QuoteIndex(), static_cast<int>(quote));
        if (slash < 16) EXPECT_EQ(block.BsIndex(), static_cast<int>(slash));
      }
    }
  }
}

TEST(RiscvScalar, QuoteExactLengthInputsAndEveryByte) {
  std::string every_byte;
  for (unsigned byte = 0; byte < 256; ++byte) {
    every_byte += static_cast<char>(byte);
  }
  std::vector<std::string> inputs = {every_byte};
  for (size_t size : kBoundaries) {
    inputs.push_back(std::string(size, 'x'));
    inputs.push_back(std::string(size, '\\'));
    inputs.push_back(std::string(size, 'x') + "\"#\\]\n");
  }
  for (size_t alignment = 0; alignment < 16; ++alignment) {
    for (const auto& input : inputs) {
      // The source ends at its allocation boundary: no padding is readable.
      std::unique_ptr<char[]> storage(new char[alignment + input.size() + 1]);
      char* source = storage.get() + alignment + 1;
      std::memcpy(source, input.data(), input.size());
      std::vector<char> output(input.size() * 6 + 64, '!');
      char* end = scalar::Quote<SerializeFlags::kSerializeDefault>(
          source, input.size(), output.data());
      EXPECT_EQ(std::string(output.data(), end), QuoteReference(input))
          << "alignment=" << alignment << " size=" << input.size();
    }
  }
}

TEST(RiscvScalar, ParseAndCopyOverlappingEscapedStrings) {
  const std::string suffixes[] = {"", "\"#\\]", "\n\t\b\f\r",
                                  std::string(80, 'q'),
                                  "\xe4\xb8\xad\xf0\x9f\x98\x80"};
  for (size_t alignment = 0; alignment < 16; ++alignment) {
    for (size_t prefix : kBoundaries) {
      for (const auto& suffix : suffixes) {
        // The first escape makes the in-place source and destination overlap.
        const std::string decoded = "\n" + std::string(prefix, 'a') + suffix;
        const std::string encoded = QuoteReference(decoded);
        std::vector<uint8_t> storage(alignment + encoded.size() + 64, 0);
        uint8_t* begin = storage.data() + alignment;
        std::memcpy(begin, encoded.data() + 1, encoded.size() - 1);
        uint8_t* source = begin;
        SonicError error = kErrorNone;
        const size_t length = scalar::parseStringInplace(source, error);
        ASSERT_EQ(error, kErrorNone);
        EXPECT_EQ(std::string(reinterpret_cast<char*>(begin), length), decoded);
        EXPECT_EQ(source, begin + encoded.size() - 1);
        EXPECT_EQ(begin[length], 0);

        Document document;
        document.Parse(encoded);
        ASSERT_FALSE(document.HasParseError());
        EXPECT_EQ(document.GetStringView(), decoded);
        WriteBuffer output;
        ASSERT_EQ(document.Serialize(output), kErrorNone);
        EXPECT_EQ(output.ToStringView(), encoded);
      }
    }
  }
}

TEST(RiscvScalar, RawControlFlagsAndUnicodeSerialization) {
  for (size_t prefix : kBoundaries) {
    for (unsigned byte = 0; byte < 32; ++byte) {
      const std::string decoded =
          std::string(prefix, 'a') + static_cast<char>(byte) + "z";
      const std::string input = "\"" + decoded + "\"";
      Document document;
      document.Parse(input);
      EXPECT_EQ(document.GetParseError(), kParseErrorUnEscaped);
      document.Parse<ParseFlags::kParseAllowUnescapedControlChars>(input);
      ASSERT_FALSE(document.HasParseError());
      EXPECT_EQ(document.GetStringView(), decoded);
      WriteBuffer output;
      ASSERT_EQ(document.Serialize(output), kErrorNone);
      EXPECT_EQ(output.ToStringView(), QuoteReference(decoded));
    }
    const std::string decoded = std::string(prefix, 'a') + "\xf0\x9f\x98\x80";
    Document document;
    document.Parse(QuoteReference(decoded));
    ASSERT_FALSE(document.HasParseError());
    WriteBuffer output;
    ASSERT_EQ(document.Serialize<SerializeFlags::kSerializeEscapeEmoji>(output),
              kErrorNone);
    EXPECT_EQ(output.ToStringView(),
              "\"" + std::string(prefix, 'a') + "\\ud83d\\ude00\"");
    constexpr auto flags = SerializeFlags::kSerializeEscapeEmoji |
                           SerializeFlags::kSerializeUnicodeEscapeUppercase;
    ASSERT_EQ(document.Serialize<flags>(output), kErrorNone);
    EXPECT_EQ(output.ToStringView(),
              "\"" + std::string(prefix, 'a') + "\\uD83D\\uDE00\"");
  }
}

TEST(RiscvScalar, BoundedSkipAndTokenSearch) {
  for (size_t alignment = 0; alignment < 16; ++alignment) {
    for (size_t size : kBoundaries) {
      const std::string bodies[] = {
          std::string(size, 'x') + "\"",
          std::string(size, 'x') + "\\\"#\\\\]\"",
          std::string(size, '\\') + (size % 2 ? "x\"" : "\"")};
      for (const auto& body : bodies) {
        std::unique_ptr<uint8_t[]> storage(
            new uint8_t[alignment + body.size()]);
        uint8_t* input = storage.get() + alignment;
        std::memcpy(input, body.data(), body.size());
        size_t position = 0;
        EXPECT_EQ(scalar::SkipString(input, position, body.size()),
                  body.find('\\') == std::string::npos ? 1 : 2);
        EXPECT_EQ(position, body.size());
      }
      const std::string dangling = std::string(size, 'x') + '\\';
      std::unique_ptr<uint8_t[]> storage(
          new uint8_t[alignment + dangling.size()]);
      uint8_t* input = storage.get() + alignment;
      std::memcpy(input, dangling.data(), dangling.size());
      size_t position = 0;
      EXPECT_EQ(scalar::SkipString(input, position, dangling.size()), 0);
      position = 0;
      EXPECT_EQ(scalar::GetNextToken(input, position, dangling.size(), ",]}"),
                0);
      EXPECT_EQ(position, dangling.size());
      input[size] = ']';
      position = 0;
      EXPECT_EQ(scalar::GetNextToken(input, position, dangling.size(), ",]}"),
                ']');
      EXPECT_EQ(position, size);
    }
  }
}

#if defined(__linux__)
TEST(RiscvScalar, BoundedOperationsAtProtectedPage) {
  const long page_size = sysconf(_SC_PAGESIZE);
  ASSERT_GT(page_size, 0);
  struct Mapping {
    void* address;
    size_t size;
    ~Mapping() {
      if (address != MAP_FAILED) munmap(address, size);
    }
  } mapping{mmap(nullptr, static_cast<size_t>(page_size) * 2,
                 PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0),
            static_cast<size_t>(page_size) * 2};
  ASSERT_NE(mapping.address, MAP_FAILED);
  uint8_t* end = static_cast<uint8_t*>(mapping.address) + page_size;
  ASSERT_EQ(mprotect(end, page_size, PROT_NONE), 0);

  for (size_t size = 0; size <= 80; ++size) {
    uint8_t* input = end - size;
    std::fill(input, end, 'x');
    std::vector<char> output(size * 6 + 64);
    char* written = scalar::Quote<SerializeFlags::kSerializeDefault>(
        reinterpret_cast<const char*>(input), size, output.data());
    EXPECT_EQ(std::string(output.data(), written),
              "\"" + std::string(size, 'x') + "\"");

    size_t position = 0;
    EXPECT_EQ(scalar::SkipString(input, position, size), 0);
    position = 0;
    EXPECT_EQ(scalar::GetNextToken(input, position, size, ",]}"), 0);
    EXPECT_EQ(position, size);
    size_t bits_end = 0;
    uint64_t bits = 0;
    position = 0;
    std::fill(input, end, ' ');
    EXPECT_EQ(scalar::skip_space_safe(input, position, size, bits_end, bits),
              size == 0 ? 0 : ' ');
    EXPECT_EQ(position, size);
    if (size == 0) continue;

    input[size - 1] = ']';
    position = 0;
    EXPECT_EQ(scalar::skip_space_safe(input, position, size, bits_end, bits),
              ']');
    EXPECT_EQ(position, size);
    position = 0;
    EXPECT_EQ(scalar::GetNextToken(input, position, size, ",]}"), ']');
    EXPECT_EQ(position, size - 1);
    input[size - 1] = '"';
    position = 0;
    EXPECT_EQ(scalar::SkipString(input, position, size), 1);
    EXPECT_EQ(position, size);
    input[size - 1] = '\\';
    position = 0;
    EXPECT_EQ(scalar::SkipString(input, position, size), 0);
    written = scalar::Quote<SerializeFlags::kSerializeDefault>(
        reinterpret_cast<const char*>(input), size, output.data());
    EXPECT_EQ(
        std::string(output.data(), written),
        QuoteReference(std::string(reinterpret_cast<char*>(input), size)));
  }
}
#endif

TEST(RiscvScalar, OnDemandAcrossWhitespaceAndEscapedContainerBoundaries) {
  for (size_t alignment = 0; alignment < 16; ++alignment) {
    for (size_t prefix : kBoundaries) {
      const std::string skipped = std::string(prefix, 'x') + "\\\"#]}[";
      const std::string spaces(prefix, ' ');
      const std::string input = "[{\"skip\":" + QuoteReference(skipped) + "}," +
                                spaces + "\t\r\n{\"want\":42}]";
      std::unique_ptr<char[]> storage(new char[alignment + input.size()]);
      char* source = storage.get() + alignment;
      std::memcpy(source, input.data(), input.size());
      StringView target;
      const auto result = GetOnDemand(StringView(source, input.size()),
                                      JsonPointer{1, "want"}, target);
      ASSERT_EQ(result.Error(), kErrorNone);
      EXPECT_EQ(target, "42");
      Document document;
      document.ParseOnDemand(source, input.size(), JsonPointer{1, "want"});
      ASSERT_FALSE(document.HasParseError());
      ASSERT_TRUE(document.IsUint64());
      EXPECT_EQ(document.GetUint64(), uint64_t{42});
    }
  }
}

TEST(RiscvScalar, QuoteEveryWordPrefix) {
  const uint8_t specials[] = {0, 1, 8, 9, 10, 12, 13, 31, '"', '\\'};
  for (size_t alignment = 0; alignment < 16; ++alignment) {
    for (size_t prefix = 0; prefix < 8; ++prefix) {
      for (uint8_t byte : specials) {
        const std::string input = std::string(prefix, 'x') +
                                  static_cast<char>(byte) +
                                  std::string(16, 'z');
        std::unique_ptr<char[]> storage(new char[alignment + input.size()]);
        char* source = storage.get() + alignment;
        std::memcpy(source, input.data(), input.size());
        std::vector<char> output(input.size() * 6 + 32);
        char* end = scalar::Quote<SerializeFlags::kSerializeDefault>(
            source, input.size(), output.data());
        EXPECT_EQ(std::string(output.data(), end), QuoteReference(input))
            << "alignment=" << alignment << " prefix=" << prefix
            << "byte=" << static_cast<unsigned>(byte);
      }
      const std::string input =
          std::string(prefix, 'x') + "\xf0\x9f\x98\x80" + std::string(16, 'z');
      std::unique_ptr<char[]> storage(new char[alignment + input.size()]);
      char* source = storage.get() + alignment;
      std::memcpy(source, input.data(), input.size());
      std::vector<char> output(input.size() * 6 + 32);
      char* end = scalar::Quote<SerializeFlags::kSerializeEscapeEmoji>(
          source, input.size(), output.data());
      EXPECT_EQ(std::string(output.data(), end),
                "\"" + std::string(prefix, 'x') + "\\ud83d\\ude00" +
                    std::string(16, 'z') + "\"");
    }
  }
}

TEST(RiscvScalar, SkipStringStopsAtFirstUnescapedQuote) {
  for (size_t alignment = 0; alignment < 16; ++alignment) {
    for (size_t prefix = 0; prefix < 32; ++prefix) {
      const std::string bodies[] = {
          std::string(prefix, 'x') + "\"" + std::string(32, '\\'),
          std::string(prefix, 'x') + "\\\"x\"" + std::string(32, '\\'),
          std::string(prefix, 'x') + "\\\"" + std::string(32, 'x')};
      const int results[] = {1, 2, 0};
      const size_t ends[] = {prefix + 1, prefix + 4, 0};
      for (size_t i = 0; i < 3; ++i) {
        std::unique_ptr<uint8_t[]> storage(
            new uint8_t[alignment + bodies[i].size()]);
        uint8_t* input = storage.get() + alignment;
        std::memcpy(input, bodies[i].data(), bodies[i].size());
        size_t position = 0;
        EXPECT_EQ(scalar::SkipString(input, position, bodies[i].size()),
                  results[i]);
        if (results[i] != 0) EXPECT_EQ(position, ends[i]);
      }
    }
  }
}

TEST(RiscvScalar, PrefixXorMatchesBitParity) {
  uint64_t state = 0x123456789abcdef0ULL;
  for (size_t i = 0; i < 4096; ++i) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    const uint64_t word = i < 64 ? (uint64_t{1} << i) : state;
    uint64_t expected = 0;
    uint64_t parity = 0;
    for (size_t bit = 0; bit < 64; ++bit) {
      parity ^= (word >> bit) & 1;
      expected |= parity << bit;
    }
    EXPECT_EQ(scalar::PrefixXor(word), expected);
  }
  EXPECT_EQ(scalar::PrefixXor(0), uint64_t{0});
  EXPECT_EQ(scalar::PrefixXor(UINT64_MAX), 0x5555555555555555ULL);
}

TEST(RiscvScalar, FindMemberBoundedShortKeys) {
  std::vector<size_t> lengths;
  for (size_t length = 0; length <= 40; ++length) lengths.push_back(length);
  lengths.push_back(64);
  for (size_t length : lengths) {
    for (bool binary : {false, true}) {
      SCOPED_TRACE(::testing::Message()
                   << "length=" << length << " binary=" << binary);
      std::string key(length, 'a');
      const unsigned char bytes[] = {0, 0x7f, 0x80, 0xff, 'x'};
      for (size_t i = 0; i < length; ++i) {
        key[i] = binary ? static_cast<char>(bytes[i % sizeof(bytes)])
                        : static_cast<char>('a' + i % 26);
      }
      Document document;
      document.SetObject();
      ASSERT_NE(document.AddMember(StringView(key.data(), key.size()),
                                   Node(uint64_t{42}), document.GetAllocator()),
                document.MemberEnd());
      const Document& const_document = document;
      for (bool mapped : {false, true}) {
        SCOPED_TRACE(::testing::Message() << "mapped=" << mapped);
        if (mapped) ASSERT_TRUE(document.CreateMap(document.GetAllocator()));
        for (size_t alignment = 0; alignment < 16; ++alignment) {
          SCOPED_TRACE(::testing::Message() << "alignment=" << alignment);
          // No readable byte follows the query, including an empty query.
          std::unique_ptr<char[]> storage(new char[alignment + length]);
          char* query = storage.get() + alignment;
          if (length) std::memcpy(query, key.data(), length);
          const auto found = document.FindMember(query, length);
          ASSERT_NE(found, document.MemberEnd());
          EXPECT_EQ(found->value.GetUint64(), uint64_t{42});
          const auto const_found = const_document.FindMember(query, length);
          ASSERT_NE(const_found, const_document.MemberEnd());
          EXPECT_EQ(const_found->value.GetUint64(), uint64_t{42});
          for (size_t mismatch = 0; mismatch < length; ++mismatch) {
            query[mismatch] ^= static_cast<char>(0x80);
            EXPECT_EQ(document.FindMember(query, length), document.MemberEnd())
                << "mismatch=" << mismatch;
            EXPECT_EQ(const_document.FindMember(query, length),
                      const_document.MemberEnd())
                << "mismatch=" << mismatch;
            query[mismatch] = key[mismatch];
          }
        }
      }
    }
  }
}

TEST(RiscvScalar, AtPointerAndStringViewUseBoundedKeys) {
  const auto check = [](char* stored, char* query, size_t length) {
    SCOPED_TRACE(::testing::Message() << "length=" << length);
    for (size_t i = 0; i < length; ++i) {
      stored[i] = query[i] = static_cast<char>('a' + i % 26);
    }
    if (length) stored[length / 2] = query[length / 2] = '\0';
    Document document;
    document.SetObject();
    // Keep both the stored key and query bounded, without copied key padding.
    ASSERT_NE(document.AddMember(StringView(stored, length), Node(uint64_t{42}),
                                 document.GetAllocator(), false),
              document.MemberEnd());
    const Document& constant = document;
    const Node* expected = &document.MemberBegin()->value;
    const StringView view(query, length);
    const JsonPointer owned{std::string(query, length)};
    const GenericJsonPointer<StringView> borrowed{view};
    for (bool mapped : {false, true}) {
      SCOPED_TRACE(::testing::Message() << "mapped=" << mapped);
      if (mapped) ASSERT_TRUE(document.CreateMap(document.GetAllocator()));
      EXPECT_EQ(document.FindMember(view), document.MemberBegin());
      EXPECT_EQ(constant.FindMember(view), constant.MemberBegin());
      EXPECT_EQ(document.AtPointer(owned), expected);
      EXPECT_EQ(constant.AtPointer(owned), expected);
      EXPECT_EQ(document.AtPointer(borrowed), expected);
      EXPECT_EQ(constant.AtPointer(borrowed), expected);
      EXPECT_EQ(document.AtPointer(view), expected);
      EXPECT_EQ(constant.AtPointer(view), expected);
      EXPECT_EQ(document.AtPointer("absent"), nullptr);
      if (length) {
        // A mismatch after an embedded NUL must not compare equal.
        query[length - 1] ^= static_cast<char>(0x80);
        EXPECT_EQ(document.FindMember(view), document.MemberEnd());
        EXPECT_EQ(constant.FindMember(view), constant.MemberEnd());
        EXPECT_EQ(document.AtPointer(borrowed), nullptr);
        EXPECT_EQ(constant.AtPointer(borrowed), nullptr);
        EXPECT_EQ(document.AtPointer(view), nullptr);
        query[length - 1] ^= static_cast<char>(0x80);
        EXPECT_EQ(document.AtPointer(StringView(query, length - 1)), nullptr);
      }
    }
  };
  const size_t lengths[] = {0, 1, 2, 3, 4, 7, 8, 15, 16, 17, 31, 32, 33, 65};
  for (size_t length : lengths) {
    for (size_t alignment = 0; alignment < 16; ++alignment) {
      SCOPED_TRACE(::testing::Message() << "alignment=" << alignment);
      std::unique_ptr<char[]> stored(new char[alignment + length]);
      std::unique_ptr<char[]> query(new char[15 - alignment + length]);
      check(stored.get() + alignment, query.get() + (15 - alignment), length);
    }
  }
#if defined(__linux__)
  const long page_size = sysconf(_SC_PAGESIZE);
  ASSERT_GT(page_size, 0);
  struct Mapping {
    void* address;
    size_t size;
    ~Mapping() {
      if (address != MAP_FAILED) munmap(address, size);
    }
  } mapping{mmap(nullptr, static_cast<size_t>(page_size) * 4,
                 PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0),
            static_cast<size_t>(page_size) * 4};
  ASSERT_NE(mapping.address, MAP_FAILED);
  char* stored_end = static_cast<char*>(mapping.address) + page_size;
  char* query_end = stored_end + 2 * page_size;
  ASSERT_EQ(mprotect(stored_end, page_size, PROT_NONE), 0);
  ASSERT_EQ(mprotect(query_end, page_size, PROT_NONE), 0);
  for (size_t length : lengths) {
    check(stored_end - length, query_end - length, length);
  }
#endif
}

TEST(RiscvScalar, SmallContainersPreserveAllMembers) {
  for (size_t count = 0; count <= 4; ++count) {
    SCOPED_TRACE(::testing::Message() << "count=" << count);
    std::string array = "[", object = "{";
    for (size_t i = 0; i < count; ++i) {
      if (i) {
        array += ',';
        object += ',';
      }
      const std::string value = std::to_string(i * 17 + 3);
      array += value;
      object += "\"key" + std::to_string(i) + "\":" + value;
    }
    array += ']';
    object += '}';
    const std::string nested = "[" + array + ',' + object +
                               ",{\"child\":" + array + "},[" + object + "]]";
    for (const std::string& input : {array, object, nested}) {
      SCOPED_TRACE(input);
      Document document;
      document.Parse(input);
      ASSERT_FALSE(document.HasParseError());
      ASSERT_TRUE(document.IsContainer());
      EXPECT_EQ(document.Size(), input == nested ? size_t{4} : count);
      if (input == nested) {
        EXPECT_EQ(document[0].Size(), count);
        EXPECT_EQ(document[1].Size(), count);
        EXPECT_EQ(document[2]["child"].Size(), count);
        EXPECT_EQ(document[3][0].Size(), count);
      } else {
        for (size_t i = 0; i < count; ++i) {
          if (document.IsArray()) {
            EXPECT_EQ(document[i].GetUint64(), i * 17 + 3);
          } else {
            const std::string key = "key" + std::to_string(i);
            const auto member = document.FindMember(key.data(), key.size());
            ASSERT_NE(member, document.MemberEnd());
            EXPECT_EQ(member->value.GetUint64(), i * 17 + 3);
          }
        }
      }
      WriteBuffer output;
      ASSERT_EQ(document.Serialize(output), kErrorNone);
      EXPECT_EQ(std::string(output.ToString(), output.Size()), input);
      Document roundtrip;
      roundtrip.Parse(output.ToString(), output.Size());
      ASSERT_FALSE(roundtrip.HasParseError());
      EXPECT_EQ(roundtrip, document);
    }
  }
}

TEST(RiscvScalar, PublicQuoteBoundedShortTails) {
  const uint8_t bytes[] = {0,   1,    8,    9,    10,   12,   13,  31,
                           '"', '\\', 0x7f, 0x80, 0xef, 0xf0, 0xff};
  const auto check_default = [](const std::string& input, size_t alignment) {
    std::unique_ptr<char[]> storage(new char[alignment + input.size() + 1]);
    char* source = storage.get() + alignment + 1;
    std::memcpy(source, input.data(), input.size());
    Node node{
        StringView(source, input.size())};  // Does not copy or add padding.
    WriteBuffer output;
    ASSERT_EQ(node.Serialize(output), kErrorNone);
    EXPECT_EQ(output.ToStringView(), QuoteReference(input));
  };
  for (size_t alignment = 0; alignment < 16; ++alignment) {
    for (size_t length = 0; length < 24; ++length) {
      std::string input(length, 'x');
      check_default(input, alignment);
      const size_t tail_begin = length - (length & 7);
      for (size_t position = tail_begin; position < length; ++position) {
        for (uint8_t byte : bytes) {
          SCOPED_TRACE(::testing::Message()
                       << "alignment=" << alignment << "length=" << length
                       << "position=" << position
                       << " byte=" << unsigned(byte));
          input[position] = static_cast<char>(byte);
          check_default(input, alignment);
        }
        input[position] = 'x';
      }
    }
    for (size_t prefix = 0; prefix < 8; ++prefix) {
      for (size_t suffix = 0; suffix < 8; ++suffix) {
        const std::string input = std::string(prefix, 'x') +
                                  "\xf0\x9f\x98\x80" + std::string(suffix, 'y');
        std::unique_ptr<char[]> storage(new char[alignment + input.size() + 1]);
        char* source = storage.get() + alignment + 1;
        std::memcpy(source, input.data(), input.size());
        Node node{StringView(source, input.size())};
        WriteBuffer output;
        ASSERT_EQ(node.Serialize<SerializeFlags::kSerializeEscapeEmoji>(output),
                  kErrorNone);
        EXPECT_EQ(output.ToStringView(), "\"" + std::string(prefix, 'x') +
                                             "\\ud83d\\ude00" +
                                             std::string(suffix, 'y') + "\"");
        constexpr auto upper = SerializeFlags::kSerializeEscapeEmoji |
                               SerializeFlags::kSerializeUnicodeEscapeUppercase;
        ASSERT_EQ(node.Serialize<upper>(output), kErrorNone);
        EXPECT_EQ(output.ToStringView(), "\"" + std::string(prefix, 'x') +
                                             "\\uD83D\\uDE00" +
                                             std::string(suffix, 'y') + "\"");
      }
    }
    // Preserve existing behavior for an incomplete four-byte sequence.
    for (size_t length = 1; length < 4; ++length) {
      const std::string input("\xf0\x9f\x98", length);
      std::unique_ptr<char[]> storage(new char[alignment + length + 1]);
      char* source = storage.get() + alignment + 1;
      std::memcpy(source, input.data(), length);
      Node node{StringView(source, length)};
      WriteBuffer output;
      ASSERT_EQ(node.Serialize<SerializeFlags::kSerializeEscapeEmoji>(output),
                kErrorNone);
      EXPECT_EQ(output.ToStringView(), QuoteReference(input));
    }
  }
}

}  // namespace
