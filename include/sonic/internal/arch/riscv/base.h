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

#include <sonic/macro.h>

#include <cstdint>
#include <cstring>

namespace sonic_json {
namespace internal {
namespace riscv {

// Some callers pass zero but do not use the result; suppress the UBSAN check.
sonic_force_inline __attribute__((no_sanitize("undefined"))) int TrailingZeroes(
    uint64_t input_num) {
  return __builtin_ctzll(input_num);
}

sonic_force_inline uint64_t ClearLowestBit(uint64_t input_num) {
  return input_num & (input_num - 1);
}

/* result might be undefined when input_num is zero */
sonic_force_inline int LeadingZeroes(uint64_t input_num) {
  return __builtin_clzll(input_num);
}

sonic_force_inline long long int CountOnes(uint64_t input_num) {
  return __builtin_popcountll(input_num);
}

sonic_force_inline uint64_t PrefixXor(uint64_t bitmask) {
#if defined(__riscv) && (__riscv_xlen == 64) && defined(__riscv_zbc)
  // Carry-less multiplication by all ones forms the inclusive prefix XOR.
  uint64_t result;
  __asm__("clmul %0, %1, %2" : "=r"(result) : "r"(bitmask), "r"(~uint64_t{0}));
  return result;
#else
  bitmask ^= bitmask << 1;
  bitmask ^= bitmask << 2;
  bitmask ^= bitmask << 4;
  bitmask ^= bitmask << 8;
  bitmask ^= bitmask << 16;
  bitmask ^= bitmask << 32;
  return bitmask;
#endif
}

template <size_t ChunkSize>
sonic_force_inline void Xmemcpy(void* dst_, const void* src_, size_t chunks) {
#if defined(__riscv_misaligned_fast)
  if (ChunkSize == 16 || ChunkSize == 32) {
    if (chunks <= 2) {
      if (chunks == 0) return;
      auto* dst = static_cast<uint8_t*>(dst_);
      const auto* src = static_cast<const uint8_t*>(src_);
      std::memcpy(dst, src, ChunkSize);
      if (chunks == 2) std::memcpy(dst + ChunkSize, src + ChunkSize, ChunkSize);
      return;
    }
  }
#endif
  std::memcpy(dst_, src_, chunks * ChunkSize);
}

#if defined(__riscv_misaligned_fast)
template <typename T>
sonic_force_inline T MemcmpWordDifference(const uint8_t* a, const uint8_t* b) {
  T left, right;
  std::memcpy(&left, a, sizeof(T));
  std::memcpy(&right, b, sizeof(T));
  return left ^ right;
}

#endif

sonic_force_inline bool InlinedMemcmpEq(const void* _a, const void* _b,
                                        size_t s) {
#if defined(__riscv_misaligned_fast)
  if (s > 32) return std::memcmp(_a, _b, s) == 0;
  const auto* a = static_cast<const uint8_t*>(_a);
  const auto* b = static_cast<const uint8_t*>(_b);
  // Overlapping head/tail loads cover short keys without reading padding.
  if (s > 16) {
    return (MemcmpWordDifference<uint64_t>(a, b) |
            MemcmpWordDifference<uint64_t>(a + 8, b + 8) |
            MemcmpWordDifference<uint64_t>(a + s - 16, b + s - 16) |
            MemcmpWordDifference<uint64_t>(a + s - 8, b + s - 8)) == 0;
  }
  if (s >= 8) {
    return (MemcmpWordDifference<uint64_t>(a, b) |
            MemcmpWordDifference<uint64_t>(a + s - 8, b + s - 8)) == 0;
  }
  if (s >= 4) {
    return (MemcmpWordDifference<uint32_t>(a, b) |
            MemcmpWordDifference<uint32_t>(a + s - 4, b + s - 4)) == 0;
  }
  if (s >= 2) {
    return (MemcmpWordDifference<uint16_t>(a, b) |
            MemcmpWordDifference<uint16_t>(a + s - 2, b + s - 2)) == 0;
  }
  return s == 0 || *a == *b;
#else
  return std::memcmp(_a, _b, s) == 0;
#endif
}

sonic_force_inline int InlinedMemcmp(const void* _l, const void* _r, size_t s) {
  return std::memcmp(_l, _r, s);
}

}  // namespace riscv
}  // namespace internal
}  // namespace sonic_json
