// Copyright 2018-2019 The simdjson authors

// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0

// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// This file may have been modified by ByteDance authors. All ByteDance
// Modifications are Copyright 2022 ByteDance Authors.

#pragma once

#include <sonic/dom/flags.h>
#include <sonic/macro.h>

#include <cstdint>

#include "../common/unicode_common.h"
#include "base.h"
#include "simd.h"

namespace sonic_json {
namespace internal {
namespace riscv {

using sonic_json::internal::common::handle_unicode_codepoint;

struct StringBlock {
 public:
  template <bool CheckEmpty = true>
  sonic_force_inline static StringBlock Find(const uint8_t* src);
  sonic_force_inline static StringBlock Find(simd8<uint8_t>& v);
  template <ParseFlags parseFlags>
  sonic_force_inline bool HasQuoteFirst() const {
    constexpr bool kAllowUnescapedControlChars =
        (parseFlags & ParseFlags::kParseAllowUnescapedControlChars) != 0;
    if constexpr (kAllowUnescapedControlChars) {
      return (((bs_bits - 1) & quote_bits) != 0);
    } else {
      return (((bs_bits - 1) & quote_bits) != 0) && (!HasUnescaped());
    }
  }
  sonic_force_inline bool HasBackslash() const {
    return ((quote_bits - 1) & bs_bits) != 0;
  }
  sonic_force_inline bool HasUnescaped() const {
    return ((quote_bits - 1) & unescaped_bits) != 0;
  }
  sonic_force_inline int QuoteIndex() const {
    return TrailingZeroes(quote_bits);
  }
  sonic_force_inline int BsIndex() const { return TrailingZeroes(bs_bits); }

  uint64_t bs_bits;
  uint64_t quote_bits;
  uint64_t unescaped_bits;
};

// Scanner masks use one bit per byte; the SIMD facade uses four.
template <bool CheckEmpty>
sonic_force_inline StringBlock StringBlock::Find(const uint8_t* src) {
  const uint64_t lo = swar::load_u64(src);
  const uint64_t hi = swar::load_u64(src + 8);
  const uint64_t bs_lo = swar::eq_byte_msb(lo, '\\');
  const uint64_t bs_hi = swar::eq_byte_msb(hi, '\\');
  const uint64_t quote_lo = swar::eq_byte_msb(lo, '"');
  const uint64_t quote_hi = swar::eq_byte_msb(hi, '"');
  const uint64_t control_lo = swar::control_msb(lo);
  const uint64_t control_hi = swar::control_msb(hi);
  if constexpr (CheckEmpty) {
    if ((bs_lo | bs_hi | quote_lo | quote_hi | control_lo | control_hi) == 0) {
      return {0, 0, 0};
    }
  }
  return {
      swar::byte_msb_to_mask(bs_lo) |
          (uint64_t(swar::byte_msb_to_mask(bs_hi)) << 8),
      swar::byte_msb_to_mask(quote_lo) |
          (uint64_t(swar::byte_msb_to_mask(quote_hi)) << 8),
      swar::byte_msb_to_mask(control_lo) |
          (uint64_t(swar::byte_msb_to_mask(control_hi)) << 8),
  };
}

sonic_force_inline StringBlock StringBlock::Find(simd8<uint8_t>& v) {
  return Find(v.data);
}

sonic_force_inline uint64_t GetNonSpaceBits(const uint8_t* data) {
  return swar::byte_msb_to_mask(swar::nonspace_msb(swar::load_u64(data))) |
         (uint64_t(swar::byte_msb_to_mask(
              swar::nonspace_msb(swar::load_u64(data + 8))))
          << 8);
}

sonic_force_inline bool IsAscii(const simd8x64<uint8_t>& input) {
  simd8<uint8_t> bits = input.reduce_or();
  return bits.max_val() < 0x80u;
}

sonic_force_inline simd8<bool> MustBe2_3Continuation(
    const simd8<uint8_t> prev2, const simd8<uint8_t> prev3) {
  simd8<bool> is_third_byte = prev2 >= uint8_t(0xe0u);
  simd8<bool> is_fourth_byte = prev3 >= uint8_t(0xf0u);
  return is_third_byte ^ is_fourth_byte;
}

}  // namespace riscv
}  // namespace internal
}  // namespace sonic_json
