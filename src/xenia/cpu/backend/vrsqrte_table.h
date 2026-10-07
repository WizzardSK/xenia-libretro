/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_BACKEND_VRSQRTE_TABLE_H_
#define XENIA_CPU_BACKEND_VRSQRTE_TABLE_H_

#include <cstdint>

namespace xe {
namespace cpu {
namespace backend {

// The 32 coefficients the guest vrsqrtefp estimate interpolates between. Each
// packs a base in the low 16 bits and a slope in the high 16.
extern const uint32_t kVRsqrteCoefficients[32];

// For positive normal inputs the estimate ignores the low 9 mantissa bits, so
// the remaining 14 mantissa bits and the low exponent bit fully determine both
// the estimated mantissa and its exponent adjustment. That is 32768 cases, few
// enough to precompute the interpolation for all of them.
//
// Indexed by bits 9..23 of the input. The entry carries a canonical exponent,
// so the caller subtracts ((input >> 24) - 63) << 23 to reach the real one.
// Special values and denormals are not covered and still need the full path.
//
// The table is 128KiB and built once on first call.
const uint32_t* GetNormalVRsqrteTable();

}  // namespace backend
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_BACKEND_VRSQRTE_TABLE_H_
