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

#include <cstdint>
#include <cstring>

namespace sonic_json {
namespace internal {
namespace riscv {

static const char kDigitPairs[201] sonic_align(2) =
    "00010203040506070809"
    "10111213141516171819"
    "20212223242526272829"
    "30313233343536373839"
    "40414243444546474849"
    "50515253545556575859"
    "60616263646566676869"
    "70717273747576777879"
    "80818283848586878889"
    "90919293949596979899";

sonic_static_inline void Write8Digits(uint32_t value, char* out) {
  const uint32_t high = value / 10000;
  const uint32_t low = value - high * 10000;
  const uint32_t a = high / 100;
  const uint32_t b = high - a * 100;
  const uint32_t c = low / 100;
  const uint32_t d = low - c * 100;
  std::memcpy(out, kDigitPairs + a * 2, 2);
  std::memcpy(out + 2, kDigitPairs + b * 2, 2);
  std::memcpy(out + 4, kDigitPairs + c * 2, 2);
  std::memcpy(out + 6, kDigitPairs + d * 2, 2);
}

sonic_static_inline char* Utoa_8(uint32_t value, char* out) {
  Write8Digits(value, out);
  return out + 8;
}

sonic_static_inline char* Utoa_16(uint64_t value, char* out) {
  const uint32_t high = static_cast<uint32_t>(value / 100000000);
  const uint32_t low =
      static_cast<uint32_t>(value - uint64_t(high) * 100000000);
  Write8Digits(high, out);
  Write8Digits(low, out + 8);
  return out + 16;
}

}  // namespace riscv
}  // namespace internal
}  // namespace sonic_json
