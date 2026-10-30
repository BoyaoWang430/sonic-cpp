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

#pragma once

#define VEC_LEN 16

#include <sonic/error.h>
#include <sonic/macro.h>

#include <cstring>

#include "../common/quote_common.h"
#include "../common/quote_tables.h"
#include "base.h"
#include "simd.h"
#include "unicode.h"

namespace sonic_json {
namespace internal {
namespace riscv {

using sonic_json::internal::common::handle_unicode_codepoint;

// Copy the zero-to-seven ordinary bytes preceding an escape.
sonic_force_inline void CopyQuotePrefix(const uint8_t* src, uint8_t* dst,
                                        size_t count) {
  if (count & 4) {
    std::memcpy(dst, src, 4);
    src += 4;
    dst += 4;
  }
  if (count & 2) {
    std::memcpy(dst, src, 2);
    src += 2;
    dst += 2;
  }
  if (count & 1) *dst = *src;
}

template <bool EscapeEmoji>
sonic_force_inline uint64_t EscapeMask(uint64_t word) {
  uint64_t mask = swar::eq_byte_msb(word, '\\') | swar::eq_byte_msb(word, '"') |
                  swar::control_msb(word);
  if constexpr (EscapeEmoji) {
    mask |= swar::zero_byte_msb(~word & 0xF0F0F0F0F0F0F0F0ULL);
  }
  return mask;
}

// Quote views may end at the first inaccessible byte. Read only full tail
// words, and discard classification bits outside each two/four-byte word.
template <typename Word, SerializeFlags serializeFlags>
sonic_force_inline void QuoteTailWords(const char*& src, size_t& nb,
                                       char*& dst) {
  constexpr bool EscapeEmoji =
      serializeFlags & SerializeFlags::kSerializeEscapeEmoji;
  constexpr uint64_t valid = sizeof(Word) == 4 ? 0x80808080ULL : 0x8080ULL;
  while (nb >= sizeof(Word)) {
    Word word;
    std::memcpy(&word, src, sizeof(Word));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    if constexpr (sizeof(Word) == 4)
      word = __builtin_bswap32(word);
    else
      word = __builtin_bswap16(word);
#endif
    const uint64_t mask = EscapeMask<EscapeEmoji>(word) & valid;
    if (mask == 0) {
      std::memcpy(dst, src, sizeof(Word));
      src += sizeof(Word);
      dst += sizeof(Word);
      nb -= sizeof(Word);
    } else {
      const size_t count = TrailingZeroes(mask) >> 3;
      CopyQuotePrefix(reinterpret_cast<const uint8_t*>(src),
                      reinterpret_cast<uint8_t*>(dst), count);
      src += count;
      dst += count;
      nb -= count;
      DoEscape<serializeFlags>(src, dst, nb);
    }
  }
}
template <SerializeFlags serializeFlags>
sonic_static_inline char* Quote(const char* src, size_t nb, char* dst) {
  constexpr bool EscapeEmoji =
      serializeFlags & SerializeFlags::kSerializeEscapeEmoji;
  *dst++ = '"';
  sonic_assert(nb < (1ULL << 32));
  while (nb >= 8) {
    const uint64_t word = swar::load_u64(reinterpret_cast<const uint8_t*>(src));
    const uint64_t mask = EscapeMask<EscapeEmoji>(word);
    if (mask == 0) {
      std::memcpy(dst, src, 8);
      src += 8;
      dst += 8;
      nb -= 8;
    } else {
      const size_t count = TrailingZeroes(mask) >> 3;
      CopyQuotePrefix(reinterpret_cast<const uint8_t*>(src),
                      reinterpret_cast<uint8_t*>(dst), count);
      src += count;
      dst += count;
      nb -= count;
      DoEscape<serializeFlags>(src, dst, nb);
    }
  }
  QuoteTailWords<uint32_t, serializeFlags>(src, nb, dst);
  QuoteTailWords<uint16_t, serializeFlags>(src, nb, dst);
  while (nb > 0) {
    const uint8_t ch = static_cast<uint8_t>(*src);
    if (kNeedEscaped[ch] || (EscapeEmoji && ch >= 0xF0)) {
      DoEscape<serializeFlags>(src, dst, nb);
    } else {
      *dst++ = *src++;
      --nb;
    }
  }
  *dst++ = '"';
  return dst;
}

template <ParseFlags parseFlags = ParseFlags::kParseDefault>
sonic_force_inline size_t parseStringInplace(uint8_t*& src, SonicError& err) {
#define SONIC_REPEAT8(v) \
  {                      \
    v v v v v v v v      \
  }
  constexpr bool kAllowUnescapedControlChars =
      (parseFlags & ParseFlags::kParseAllowUnescapedControlChars) != 0;
  uint8_t* dst = src;
  uint8_t* sdst = src;
  // Short strings usually end in the first block; avoid an empty-block test.
  auto block = StringBlock::Find<false>(src);
  while (1) {
  find:
    if (block.HasQuoteFirst<parseFlags>()) {
      int idx = block.QuoteIndex();
      src += idx;
      *src++ = '\0';
      return src - sdst - 1;
    }
    if constexpr (!kAllowUnescapedControlChars) {
      if (block.HasUnescaped()) {
        err = kParseErrorUnEscaped;
        return 0;
      }
    }
    if (!block.HasBackslash()) {
      src += VEC_LEN;
      block = StringBlock::Find(src);
      goto find;
    }

    auto bs_dist = block.BsIndex();
    src += bs_dist;
    dst = src;
  cont:
    uint8_t escape_char = src[1];
    if (sonic_unlikely(escape_char == 'u')) {
      if (!handle_unicode_codepoint(const_cast<const uint8_t**>(&src), &dst)) {
        err = kParseErrorEscapedUnicode;
        return 0;
      }
    } else {
      *dst = kEscapedMap[escape_char];
      if (sonic_unlikely(*dst == 0u)) {
        err = kParseErrorEscapedFormat;
        return 0;
      }
      src += 2;
      dst += 1;
    }
    if (*src == '\\') {
      bs_dist = 0;
      goto cont;
    }

  find_and_move:
    // Copy the next n bytes, and find the backslash and quote in them.
    simd8<uint8_t> v = simd8<uint8_t>::load(src);
    block = StringBlock::Find(v);
    if (block.HasQuoteFirst<parseFlags>()) {
      while (1) {
        SONIC_REPEAT8(if (sonic_unlikely(*src == '"')) break;
                      else { *dst++ = *src++; });
      }
      *dst = '\0';
      src++;
      return dst - sdst;
    }
    if constexpr (!kAllowUnescapedControlChars) {
      if (block.HasUnescaped()) {
        err = kParseErrorUnEscaped;
        return 0;
      }
    }
    if (!block.HasBackslash()) {
      v.store(dst);
      src += VEC_LEN;
      dst += VEC_LEN;
      goto find_and_move;
    }
    while (1) {
      SONIC_REPEAT8(if (sonic_unlikely(*src == '\\')) break;
                    else { *dst++ = *src++; });
    }
    goto cont;
  }
#undef SONIC_REPEAT8
}

}  // namespace riscv
}  // namespace internal
}  // namespace sonic_json

#undef VEC_LEN
