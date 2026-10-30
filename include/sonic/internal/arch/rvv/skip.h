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

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include <sonic/internal/arch/common/skip_common.h>
#include <sonic/internal/utils.h>

#include "base.h"
#include "simd.h"

namespace sonic_json {
namespace internal {
namespace rvv {

using common::EqBytes4;
using common::SkipLiteral;

namespace skip_detail {

// Carry describes an unpaired backslash at the last active lane, not bit 63.
sonic_force_inline uint64_t EscapedBits(uint64_t backslashes, size_t vl,
                                      uint64_t& carry) {
  constexpr uint64_t odd_bits = 0xAAAAAAAAAAAAAAAAULL;
  const uint64_t without_carry = backslashes & ~carry;
  const uint64_t runs =
      (((without_carry << 1) | odd_bits) - without_carry) ^ odd_bits;
  const uint64_t escaped = runs ^ (backslashes | carry);
  carry = ((runs & backslashes) >> (vl - 1)) & 1;
  return escaped;
}

}  // namespace skip_detail

template <size_t N>
sonic_force_inline uint8_t GetNextToken(const uint8_t* data, size_t& pos,
                                      size_t len, const char (&tokens)[N]) {
  if constexpr (N <= 1) {
    if (pos < len) pos = len;
    return 0;
  } else {
    while (pos < len) {
      const size_t vl = SetVL(std::min(len - pos, size_t{64}));
      const vuint8m4_t input = __riscv_vle8_v_u8m4(data + pos, vl);
      vbool2_t matches = __riscv_vmseq_vx_u8m4_b2(
          input, static_cast<uint8_t>(tokens[0]), vl);
      for (size_t i = 1; i + 1 < N; ++i) {
        matches = __riscv_vmor_mm_b2(
            matches,
            __riscv_vmseq_vx_u8m4_b2(
                input, static_cast<uint8_t>(tokens[i]), vl),
            vl);
      }
      const long first = __riscv_vfirst_m_b2(matches, vl);
      if (first >= 0) {
        pos += static_cast<size_t>(first);
        return data[pos];
      }
      pos += vl;
    }
    return 0;
  }
}

// The opening quote has already been consumed. Return 1 for a plain string,
// 2 for an escaped string, and 0 if no closing quote exists.
sonic_force_inline int SkipString(const uint8_t* data, size_t& pos,
                                 size_t len) {
  bool had_escape = false;
  uint64_t carry = 0;
  while (pos < len) {
    const size_t vl = SetVL(std::min(len - pos, size_t{64}));
    const vuint8m4_t input = __riscv_vle8_v_u8m4(data + pos, vl);
    const vbool2_t quote_mask = __riscv_vmseq_vx_u8m4_b2(input, '"', vl);
    const vbool2_t bs_mask = __riscv_vmseq_vx_u8m4_b2(input, '\\', vl);
    const uint64_t backslashes = MaskBits(bs_mask, vl);
    if ((backslashes | carry) == 0) {
      const long first = __riscv_vfirst_m_b2(quote_mask, vl);
      if (first >= 0) {
        pos += static_cast<size_t>(first) + 1;
        return had_escape ? 2 : 1;
      }
      pos += vl;
      continue;
    }
    const uint64_t escaped = skip_detail::EscapedBits(backslashes, vl, carry);
    const uint64_t quotes = MaskBits(quote_mask, vl) & ~escaped;
    if (quotes != 0) {
      const size_t first = static_cast<size_t>(TrailingZeroes(quotes));
      const uint64_t before_quote = (uint64_t{1} << first) - 1;
      had_escape |= (backslashes & before_quote) != 0;
      pos += first + 1;
      return had_escape ? 2 : 1;
    }
    had_escape |= backslashes != 0;
    pos += vl;
  }
  if (carry != 0) --pos;
  return 0;
}

sonic_force_inline bool SkipContainer(const uint8_t* data, size_t& pos,
                                     size_t len, uint8_t left, uint8_t right) {
  const size_t start = pos;
  size_t cursor = pos;
  size_t depth = 1;
  uint64_t carry = 0;
  uint64_t in_string_carry = 0;
  while (cursor < len) {
    const size_t vl = SetVL(std::min(len - cursor, size_t{64}));
    const vuint8m4_t input = __riscv_vle8_v_u8m4(data + cursor, vl);
    const vbool2_t quote_mask = __riscv_vmseq_vx_u8m4_b2(input, '"', vl);
    const vbool2_t bs_mask = __riscv_vmseq_vx_u8m4_b2(input, '\\', vl);
    const vbool2_t open_mask = __riscv_vmseq_vx_u8m4_b2(input, left, vl);
    const vbool2_t close_mask = __riscv_vmseq_vx_u8m4_b2(input, right, vl);
    const uint64_t backslashes = MaskBits(bs_mask, vl);
    uint64_t quotes = MaskBits(quote_mask, vl);
    uint64_t opens = MaskBits(open_mask, vl);
    uint64_t closes = MaskBits(close_mask, vl);
    const uint64_t escaped = (backslashes | carry) != 0
                                 ? skip_detail::EscapedBits(backslashes, vl, carry)
                                 : 0;
    quotes &= ~escaped;
    const uint64_t in_string = PrefixXor(quotes) ^ in_string_carry;
    in_string_carry = uint64_t{0} - ((in_string >> (vl - 1)) & 1);
    opens &= ~in_string;
    closes &= ~in_string;

    if (static_cast<size_t>(CountOnes(closes)) >= depth) {
      while (closes != 0) {
        const uint64_t preceding_opens = opens & (closes - 1);
        depth += static_cast<size_t>(CountOnes(preceding_opens));
        opens ^= preceding_opens;
        if (--depth == 0) {
          pos = cursor + static_cast<size_t>(TrailingZeroes(closes)) + 1;
          return true;
        }
        closes &= closes - 1;
      }
    }
    depth += static_cast<size_t>(CountOnes(opens));
    depth -= static_cast<size_t>(CountOnes(closes));
    cursor += vl;
  }
  // Legacy scanners leave an incomplete final 64-byte block unconsumed.
  if (start < len) pos = start + ((len - start) / 64) * 64;
  return false;
}

sonic_force_inline uint8_t skip_space(const uint8_t* data, size_t& pos,
                                    size_t&, uint64_t&) {
  if (!IsSpace(data[pos++])) return data[pos - 1];
  if (!IsSpace(data[pos++])) return data[pos - 1];
  while (true) {
    // This interface has no length. Retain its existing 16-byte lookahead.
    const size_t vl = SetVL(16);
    const vuint8m4_t input = __riscv_vle8_v_u8m4(data + pos, vl);
    const long first = __riscv_vfirst_m_b2(NonSpaceMask(input, vl), vl);
    if (first >= 0) {
      pos += static_cast<size_t>(first);
      return data[pos++];
    }
    pos += vl;
  }
}

sonic_force_inline uint8_t skip_space_safe(const uint8_t* data, size_t& pos,
                                         size_t len, size_t& nonspace_bits_end,
                                         uint64_t& nonspace_bits) {
  // A valid cache covers [nonspace_bits_end - 64, nonspace_bits_end).
  // Reset it when the input changes, including when a buffer is overwritten.
  if (pos < len && !IsSpace(data[pos])) return data[pos++];
  while (pos < len) {
    if (pos < nonspace_bits_end && nonspace_bits_end <= len &&
        nonspace_bits_end - pos <= 64) {
      const uint64_t remaining =
          nonspace_bits >> (64 - (nonspace_bits_end - pos));
      if (remaining != 0) {
        pos += static_cast<size_t>(TrailingZeroes(remaining));
        return data[pos++];
      }
      pos = nonspace_bits_end;
      continue;
    }
    if (len - pos >= 64) {
      // Cache a complete logical block so its start needs no extra state.
      // A short actual VL may require several bounded vector loads.
      uint64_t nonspace = 0;
      size_t loaded = 0;
      do {
        const size_t vl = SetVL(64 - loaded);
        const vuint8m4_t input = __riscv_vle8_v_u8m4(data + pos + loaded, vl);
        nonspace |= MaskBits(NonSpaceMask(input, vl), vl) << loaded;
        loaded += vl;
      } while (loaded < 64);
      nonspace_bits_end = pos + 64;
      nonspace_bits = nonspace;
      if (nonspace != 0) {
        pos += static_cast<size_t>(TrailingZeroes(nonspace));
        return data[pos++];
      }
      pos += 64;
      continue;
    }
    const size_t vl = SetVL(len - pos);
    const vuint8m4_t input = __riscv_vle8_v_u8m4(data + pos, vl);
    const long first = __riscv_vfirst_m_b2(NonSpaceMask(input, vl), vl);
    if (first >= 0) {
      pos += static_cast<size_t>(first);
      return data[pos++];
    }
    pos += vl;
  }
  return pos != 0 ? data[pos - 1] : 0;
}

}  // namespace rvv
}  // namespace internal
}  // namespace sonic_json
