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

#pragma once

#include <sonic/error.h>

#include "../common/quote_common.h"
#include "unicode.h"

namespace sonic_json {
namespace internal {
namespace rvv {

template <SerializeFlags serializeFlags>
sonic_static_inline char* Quote(const char* src, size_t nb, char* dst) {
  *dst++ = '"';
  while (nb != 0) {
    const size_t vl = SetVL(nb);
    const vuint8m4_t input = __riscv_vle8_v_u8m4(
        reinterpret_cast<const uint8_t*>(src), vl);
    vbool2_t special = __riscv_vmor_mm_b2(
        __riscv_vmseq_vx_u8m4_b2(input, '"', vl),
        __riscv_vmseq_vx_u8m4_b2(input, '\\', vl), vl);
    special = __riscv_vmor_mm_b2(
        special, __riscv_vmsltu_vx_u8m4_b2(input, 0x20, vl), vl);
    if constexpr (serializeFlags & SerializeFlags::kSerializeEscapeEmoji) {
      special = __riscv_vmor_mm_b2(
          special, __riscv_vmsgeu_vx_u8m4_b2(input, 0xf0, vl), vl);
    }
    const size_t prefix = FirstMatch(special, vl);
    // prefix <= vl <= VLMAX, so this store activates exactly prefix lanes.
    __riscv_vse8_v_u8m4(reinterpret_cast<uint8_t*>(dst), input, prefix);
    src += prefix;
    dst += prefix;
    nb -= prefix;
    if (prefix < vl) DoEscape<serializeFlags>(src, dst, nb);
  }
  *dst++ = '"';
  return dst;
}

template <ParseFlags parseFlags, bool useLength>
sonic_force_inline size_t parseStringInplaceImpl(uint8_t*& src, size_t remaining,
                                               SonicError& err) {
  uint8_t* const start = src;
  uint8_t* const end = src + remaining;
  uint8_t* dst = src;
  while (true) {
    // Keep the 16-byte lookahead for escape decoding and padded tails.
    const size_t vl = __riscv_vsetvl_e8m1(16);
    const vuint8m1_t input = __riscv_vle8_v_u8m1(src, vl);
    const StringBlock block = StringBlock::Find<parseFlags>(src, input, vl);
    if (block.kind == StringBlock::Kind::kQuote) {
      if (dst != src) __riscv_vse8_v_u8m1(dst, input, block.index);
      dst += block.index;
      src += block.index + 1;
      *dst = '\0';
      return static_cast<size_t>(dst - start);
    }
    if constexpr (!(parseFlags & ParseFlags::kParseAllowUnescapedControlChars)) {
      if (block.kind == StringBlock::Kind::kUnescaped) {
        err = kParseErrorUnEscaped;
        return 0;
      }
    }
    if (block.kind == StringBlock::Kind::kPlain) {
      if (dst != src) __riscv_vse8_v_u8m1(dst, input, vl);
      src += vl;
      dst += vl;
      if constexpr (useLength) {
        // Widen after 32 initial plain bytes; keep escape windows unchanged.
        while (src == dst && static_cast<size_t>(src - start) >= 32 &&
               src < end) {
          const size_t wide_vl = SetVL(static_cast<size_t>(end - src));
          const auto wide_input = __riscv_vle8_v_u8m4(src, wide_vl);
          auto special = __riscv_vmor_mm_b2(
              __riscv_vmseq_vx_u8m4_b2(wide_input, '"', wide_vl),
              __riscv_vmseq_vx_u8m4_b2(wide_input, '\\', wide_vl), wide_vl);
          if constexpr (!(parseFlags &
                          ParseFlags::kParseAllowUnescapedControlChars)) {
            special = __riscv_vmor_mm_b2(
                special,
                __riscv_vmsltu_vx_u8m4_b2(wide_input, 0x20, wide_vl), wide_vl);
          }
          const size_t first = FirstMatch(special, wide_vl);
          if (first < wide_vl && src[first] == '"') {
            src[first] = '\0';
            src += first + 1;
            return static_cast<size_t>(src - start - 1);
          }
          // Preserve the old decoder's error priority and error offsets.
          const size_t advance =
              vl == 16 ? first & ~size_t{15} : first - first % vl;
          src += advance;
          dst += advance;
          if (first != wide_vl || advance == 0) break;
        }
      }
      continue;
    }
    if (dst != src) __riscv_vse8_v_u8m1(dst, input, block.index);
    src += block.index;
    dst += block.index;
    do {
      const uint8_t escape_char = src[1];
      if (sonic_unlikely(escape_char == 'u')) {
        if (!common::handle_unicode_codepoint(
                const_cast<const uint8_t**>(&src), &dst)) {
          err = kParseErrorEscapedUnicode;
          return 0;
        }
      } else {
        *dst = kEscapedMap[escape_char];
        if (sonic_unlikely(*dst == 0)) {
          err = kParseErrorEscapedFormat;
          return 0;
        }
        src += 2;
        ++dst;
      }
    } while (*src == '\\');
  }
}

template <ParseFlags parseFlags = ParseFlags::kParseDefault>
sonic_force_inline size_t parseStringInplace(uint8_t*& src, SonicError& err) {
  return parseStringInplaceImpl<parseFlags, false>(src, 0, err);
}

// remaining bounds wide scans; the caller must still supply decoder padding.
template <ParseFlags parseFlags = ParseFlags::kParseDefault>
sonic_force_inline size_t parseStringInplacePadded(uint8_t*& src,
                                                size_t remaining,
                                                SonicError& err) {
  return parseStringInplaceImpl<parseFlags, true>(src, remaining, err);
}

}  // namespace rvv
}  // namespace internal
}  // namespace sonic_json
