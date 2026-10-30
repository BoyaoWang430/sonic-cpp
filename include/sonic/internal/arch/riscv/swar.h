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
namespace swar {

static constexpr uint64_t kByteMsb = 0x8080808080808080ULL;
static constexpr uint64_t kByteLsb = 0x0101010101010101ULL;
static constexpr uint64_t kMovemaskMultiplier = 0x0002040810204081ULL;

sonic_force_inline uint64_t load_u64(const uint8_t* ptr) {
  uint64_t value;
  std::memcpy(&value, ptr, sizeof(value));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  value = __builtin_bswap64(value);
#endif
  return value;
}

// Exact flags are required: a subtract-based zero test can borrow into the
// following byte and corrupt quote/escape state across scanner blocks.
sonic_force_inline uint64_t zero_byte_msb(uint64_t value) {
#if defined(__riscv) && (__riscv_xlen == 64) && defined(__riscv_zbb)
  uint64_t nonzero;
  __asm__("orc.b %0, %1" : "=r"(nonzero) : "r"(value));
  return ~nonzero & kByteMsb;
#else
  constexpr uint64_t low = 0x7F7F7F7F7F7F7F7FULL;
  return ~(((value & low) + low) | value | low);
#endif
}

sonic_force_inline uint64_t eq_byte_msb(uint64_t value, uint8_t byte) {
  return zero_byte_msb(value ^ (kByteLsb * byte));
}

sonic_force_inline uint64_t control_msb(uint64_t value) {
  return zero_byte_msb(value & 0xE0E0E0E0E0E0E0E0ULL);
}

sonic_force_inline uint64_t nonspace_msb(uint64_t value) {
  return kByteMsb & ~(eq_byte_msb(value, ' ') | eq_byte_msb(value, '\t') |
                      eq_byte_msb(value, '\n') | eq_byte_msb(value, '\r'));
}

sonic_force_inline uint32_t byte_msb_to_mask(uint64_t value) {
  return static_cast<uint32_t>(((value & kByteMsb) * kMovemaskMultiplier) >>
                               56);
}

sonic_force_inline uint32_t nonzero_byte_mask(uint64_t value) {
  return byte_msb_to_mask(~zero_byte_msb(value));
}

sonic_force_inline uint32_t zero_byte_mask(uint64_t value) {
  return byte_msb_to_mask(zero_byte_msb(value));
}

sonic_force_inline uint32_t bool_mask16(const uint8_t* ptr) {
  uint32_t low = nonzero_byte_mask(load_u64(ptr));
  uint32_t high = nonzero_byte_mask(load_u64(ptr + 8));
  return low | (high << 8);
}

sonic_force_inline uint32_t eq_mask16(const uint8_t* ptr, uint8_t value) {
  uint64_t repeated = kByteLsb * value;
  uint32_t low = zero_byte_mask(load_u64(ptr) ^ repeated);
  uint32_t high = zero_byte_mask(load_u64(ptr + 8) ^ repeated);
  return low | (high << 8);
}

sonic_force_inline uint32_t lteq_mask16(const uint8_t* ptr, uint8_t value) {
  if (value == 0xFFu) {
    return 0xFFFFu;
  }
  if (value == 0x1Fu) {
    constexpr uint64_t high_three_bits = 0xE0E0E0E0E0E0E0E0ULL;
    uint32_t low = zero_byte_mask(load_u64(ptr) & high_three_bits);
    uint32_t high = zero_byte_mask(load_u64(ptr + 8) & high_three_bits);
    return low | (high << 8);
  }
  if (value == 0x7Fu) {
    uint32_t low = byte_msb_to_mask(~load_u64(ptr));
    uint32_t high = byte_msb_to_mask(~load_u64(ptr + 8));
    return low | (high << 8);
  }

  uint32_t mask = 0;
  for (int i = 0; i < 16; i++) {
    if (ptr[i] <= value) mask |= (1u << i);
  }
  return mask;
}

}  // namespace swar
}  // namespace riscv
}  // namespace internal
}  // namespace sonic_json
