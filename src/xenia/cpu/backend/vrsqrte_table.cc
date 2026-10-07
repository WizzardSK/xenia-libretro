/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/vrsqrte_table.h"

#include <array>

namespace xe {
namespace cpu {
namespace backend {

const uint32_t kVRsqrteCoefficients[32] = {
    0x0568B4FD, 0x04F3AF97, 0x048DAAA5, 0x0435A618, 0x03E7A1E4, 0x03A29DFE,
    0x03659A5C, 0x032E96F8, 0x02FC93CA, 0x02D090CE, 0x02A88DFE, 0x02838B57,
    0x026188D4, 0x02438673, 0x02268431, 0x020B820B, 0x03D27FFA, 0x03807C29,
    0x033878AA, 0x02F97572, 0x02C27279, 0x02926FB7, 0x02666D26, 0x023F6AC0,
    0x021D6881, 0x01FD6665, 0x01E16468, 0x01C76287, 0x01AF60C1, 0x01995F12,
    0x01855D79, 0x01735BF4,
};

// The interpolation the backends emit for anything the table does not cover,
// evaluated here for the cases it does.
static uint32_t ComputeNormalVRsqrteTableValue(uint32_t input) {
  const uint32_t mantissa = input & 0x7FFFFF;
  const uint32_t coefficient_index =
      (((input >> 23) & 1) << 4) | (mantissa >> 19);
  const uint32_t coefficient = kVRsqrteCoefficients[coefficient_index];

  uint32_t estimate = ((coefficient << 10) & 0x3FFFC00) -
                      (((mantissa >> 9) & 1023) * (coefficient >> 16));
  int32_t output_exponent_adjustment = 0;
  if (!(estimate & 0x02000000)) {
    const uint32_t normalized_estimate = estimate & 0x1FFFFFF;
    uint32_t leading_zero_count = 0;
    for (uint32_t bit = 0x80000000; !(normalized_estimate & bit); bit >>= 1) {
      ++leading_zero_count;
    }
    output_exponent_adjustment += 6 - static_cast<int32_t>(leading_zero_count);
    estimate <<= leading_zero_count - 6;
  }
  if ((estimate & 5) && (estimate & 2)) {
    estimate += 4;
  }

  return (0x3F800000 +
          static_cast<uint32_t>(output_exponent_adjustment) * 0x00800000) |
         ((estimate >> 2) & 0x7FFFFF);
}

const uint32_t* GetNormalVRsqrteTable() {
  alignas(64) static const std::array<uint32_t, 1 << 15> table = []() {
    std::array<uint32_t, 1 << 15> values;
    for (uint32_t index = 0; index < values.size(); ++index) {
      // Any exponent with the right parity gives the same mantissa, so build
      // each entry from the canonical one the caller corrects away from.
      const uint32_t exponent_parity = index >> 14;
      const uint32_t mantissa = (index & 0x3FFF) << 9;
      const uint32_t canonical_exponent = 126 + exponent_parity;
      values[index] =
          ComputeNormalVRsqrteTableValue((canonical_exponent << 23) | mantissa);
    }
    return values;
  }();
  return table.data();
}

}  // namespace backend
}  // namespace cpu
}  // namespace xe
