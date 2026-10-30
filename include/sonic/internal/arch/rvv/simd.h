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
#include <sonic/macro.h>

#include <cstddef>
#include <cstdint>

namespace sonic_json {
namespace internal {
namespace rvv {

// e8m4 can cover a 64-byte mask block even at VLEN=128.
// Uncapped string scans can use larger VLs on wider implementations.
sonic_force_inline size_t SetVL(size_t remaining) {
  return __riscv_vsetvl_e8m4(remaining);
}

// One bit per byte. The 64-lane limit belongs to the scalar mask interface,
// not to VLEN; callers advance by the actual VL returned by SetVL.
sonic_force_inline uint64_t MaskBits(vbool2_t mask, size_t vl) {
  sonic_assert(vl <= 64);
  if (vl == 0) return 0;
  const vuint64m1_t packed = __riscv_vreinterpret_v_b2_u64m1(mask);
  const uint64_t bits = __riscv_vmv_x_s_u64m1_u64(packed);
  return vl == 64 ? bits : bits & ((uint64_t{1} << vl) - 1);
}

sonic_force_inline vbool2_t NonSpaceMask(vuint8m4_t input, size_t vl) {
  const vbool2_t spaces = __riscv_vmor_mm_b2(
      __riscv_vmseq_vx_u8m4_b2(input, ' ', vl),
      __riscv_vmseq_vx_u8m4_b2(input, '\t', vl), vl);
  const vbool2_t lines = __riscv_vmor_mm_b2(
      __riscv_vmseq_vx_u8m4_b2(input, '\n', vl),
      __riscv_vmseq_vx_u8m4_b2(input, '\r', vl), vl);
  return __riscv_vmnot_m_b2(__riscv_vmor_mm_b2(spaces, lines, vl), vl);
}

sonic_force_inline size_t FirstMatch(vbool2_t mask, size_t vl) {
  const long first = __riscv_vfirst_m_b2(mask, vl);
  return first < 0 ? vl : static_cast<size_t>(first);
}

}  // namespace rvv
}  // namespace internal
}  // namespace sonic_json
