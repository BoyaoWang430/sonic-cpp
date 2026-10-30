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

#include <riscv_vector.h>

#include "../riscv/itoa.h"

namespace sonic_json {
namespace internal {
namespace rvv {

using riscv::Utoa_8;

static sonic_force_inline char* Utoa_16(uint64_t val, char* out) {
  if (__riscv_vsetvl_e16m2(16) != 16) {
    return riscv::Utoa_16(val, out);
  }
  // As in arm_common::Utoa_4_helper, these fixed-point factors produce
  // a, ab, abc, abcd from each four-digit group.
  static const uint16_t divisors[16] = {
      0x20c5, 0x147b, 0x3334, 0x8000, 0x20c5, 0x147b, 0x3334, 0x8000,
      0x20c5, 0x147b, 0x3334, 0x8000, 0x20c5, 0x147b, 0x3334, 0x8000};
  static const uint16_t scales[16] = {
      0x0080, 0x0800, 0x2000, 0x8000, 0x0080, 0x0800, 0x2000, 0x8000,
      0x0080, 0x0800, 0x2000, 0x8000, 0x0080, 0x0800, 0x2000, 0x8000};
  const uint32_t hi = static_cast<uint32_t>(val / 100000000);
  const uint32_t lo = static_cast<uint32_t>(val % 100000000);

  // Each four-digit group occupies four consecutive lanes, independent of VLEN.
  auto groups = __riscv_vmv_v_x_u16m2(hi / 10000, 16);
  groups = __riscv_vslideup_vx_u16m2(
      groups, __riscv_vmv_v_x_u16m2(hi % 10000, 16), 4, 16);
  groups = __riscv_vslideup_vx_u16m2(
      groups, __riscv_vmv_v_x_u16m2(lo / 10000, 16), 8, 16);
  groups = __riscv_vslideup_vx_u16m2(
      groups, __riscv_vmv_v_x_u16m2(lo % 10000, 16), 12, 16);
  groups = __riscv_vsll_vx_u16m2(groups, 2, 16);

  auto products = __riscv_vwmulu_vv_u32m4(
      groups, __riscv_vle16_v_u16m2(divisors, 16), 16);
  auto prefixes = __riscv_vnsrl_wx_u16m2(products, 16, 16);
  products = __riscv_vwmulu_vv_u32m4(
      prefixes, __riscv_vle16_v_u16m2(scales, 16), 16);
  prefixes = __riscv_vnsrl_wx_u16m2(products, 16, 16);

  // Each 64-bit element contains one four-digit group. Shifting within that
  // element inserts zero at the group start without crossing group boundaries.
  const auto previous = __riscv_vreinterpret_v_u64m2_u16m2(
      __riscv_vsll_vx_u64m2(
          __riscv_vreinterpret_v_u16m2_u64m2(prefixes), 16, 4));
  const auto digits = __riscv_vsub_vv_u16m2(
      prefixes, __riscv_vmul_vx_u16m2(previous, 10, 16), 16);
  const auto bytes = __riscv_vadd_vx_u8m1(
      __riscv_vncvt_x_x_w_u8m1(digits, 16), '0', 16);
  __riscv_vse8_v_u8m1(reinterpret_cast<uint8_t*>(out), bytes, 16);
  return out + 16;
}

}  // namespace rvv
}  // namespace internal
}  // namespace sonic_json
