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

#include <sonic/macro.h>

#include <cstddef>

#include "../common/arm_common/str2int.h"
#include "swar.h"

namespace sonic_json {
namespace internal {
namespace riscv {

using sonic_json::internal::arm_common::simd_str2int;

// The caller has already verified that all eight bytes are decimal digits.
sonic_force_inline uint32_t Parse8DigitsUnchecked(const char* input) {
  uint64_t word = swar::load_u64(reinterpret_cast<const uint8_t*>(input));
  word -= 0x3030303030303030ULL;
  word = (word * 10 + (word >> 8)) & 0x00FF00FF00FF00FFULL;
  word = (word * 100 + (word >> 16)) & 0x0000FFFF0000FFFFULL;
  return static_cast<uint32_t>(word * 10000 + (word >> 32));
}

// Accumulate at most the requested number of digits, without reading beyond
// the logical input. Short fractions return before entering the word path.
sonic_force_inline uint64_t ParseFractionDigits(const char* c, int& count,
                                                std::size_t available) {
  const int capacity = available < static_cast<std::size_t>(count)
                           ? static_cast<int>(available)
                           : count;
  uint64_t sum = 0;
  int i = 0;
  for (; i < 4 && i < capacity; ++i) {
    const unsigned digit =
        static_cast<unsigned>(static_cast<uint8_t>(c[i]) - '0');
    if (digit > 9) {
      count = i;
      return sum;
    }
    sum = sum * 10 + digit;
  }
  if (i == capacity ||
      static_cast<unsigned>(static_cast<uint8_t>(c[i]) - '0') > 9) {
    count = i;
    return sum;
  }
  while (capacity - i >= 8) {
    const uint64_t word =
        swar::load_u64(reinterpret_cast<const uint8_t*>(c + i));
    if (((word + 0x4646464646464646ULL) | (word - 0x3030303030303030ULL)) &
        swar::kByteMsb)
      break;
    sum = sum * 100000000 + Parse8DigitsUnchecked(c + i);
    i += 8;
  }
  for (; i < capacity; ++i) {
    const unsigned digit =
        static_cast<unsigned>(static_cast<uint8_t>(c[i]) - '0');
    if (digit > 9) break;
    sum = sum * 10 + digit;
  }
  count = i;
  return sum;
}

}  // namespace riscv
}  // namespace internal
}  // namespace sonic_json
