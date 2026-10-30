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

#include "../riscv/base.h"

namespace sonic_json {
namespace internal {
namespace rvv {

using riscv::ClearLowestBit;
using riscv::CountOnes;
using riscv::InlinedMemcmp;
using riscv::InlinedMemcmpEq;
using riscv::LeadingZeroes;
using riscv::PrefixXor;
using riscv::TrailingZeroes;
using riscv::Xmemcpy;

}  // namespace rvv
}  // namespace internal
}  // namespace sonic_json
