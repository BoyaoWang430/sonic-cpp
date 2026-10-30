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

#include <sonic/dom/flags.h>

#include "simd.h"

namespace sonic_json {
namespace internal {
namespace rvv {

sonic_force_inline size_t FirstStringMatch(vbool8_t mask, size_t vl) {
  const long first = __riscv_vfirst_m_b8(mask, vl);
  return first < 0 ? vl : static_cast<size_t>(first);
}

struct StringBlock {
  enum class Kind { kPlain, kQuote, kBackslash, kUnescaped };

  template <ParseFlags parseFlags>
  sonic_force_inline static StringBlock Find(const uint8_t* src,
                                            vuint8m1_t input, size_t vl) {
    const vbool8_t backslashes = __riscv_vmseq_vx_u8m1_b8(input, '\\', vl);
    const vbool8_t quotes = __riscv_vmseq_vx_u8m1_b8(input, '"', vl);
    vbool8_t special = __riscv_vmor_mm_b8(backslashes, quotes, vl);
    if constexpr (!(parseFlags & ParseFlags::kParseAllowUnescapedControlChars)) {
      const vbool8_t controls = __riscv_vmsltu_vx_u8m1_b8(input, 0x20, vl);
      special = __riscv_vmor_mm_b8(special, controls, vl);
      const size_t first = FirstStringMatch(special, vl);
      if (first == vl) return {first, Kind::kPlain};
      if (src[first] == '"') return {first, Kind::kQuote};
      if (src[first] < 0x20) return {first, Kind::kUnescaped};
      // Preserve the existing priority of a raw control before the first quote,
      // even when a backslash precedes it in this lookahead block.
      const size_t control = FirstStringMatch(controls, vl);
      if (control != vl && control < FirstStringMatch(quotes, vl)) {
        return {control, Kind::kUnescaped};
      }
      return {first, Kind::kBackslash};
    } else {
      const size_t first = FirstStringMatch(special, vl);
      if (first == vl) return {first, Kind::kPlain};
      return {first, src[first] == '"' ? Kind::kQuote : Kind::kBackslash};
    }
  }

  size_t index;
  Kind kind;
};

}  // namespace rvv
}  // namespace internal
}  // namespace sonic_json
