/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/xenos.h"
#include "xenia/base/memory.h"

namespace xe {
namespace gpu {
namespace xenos {

// Based on X360GammaToLinear and X360LinearToGamma from the Source Engine, with
// additional logic from Direct3D 9 code in game executable disassembly, located
// via the floating-point constants involved.
// https://github.com/ValveSoftware/source-sdk-2013/blob/master/mp/src/mathlib/color_conversion.cpp#L329
// These are provided here in part as a reference for shader translators.

float PWLGammaToLinear(float gamma) {
  // Not found in game executables, so just using the logic similar to that in
  // the Source Engine.
  gamma = xe::saturate(gamma);
  float scale, offset;
  // While the compiled code for linear to gamma conversion uses `vcmpgtfp
  // constant, value` comparison (constant > value, or value < constant), it's
  // preferable to use `value >= constant` condition for the higher pieces, as
  // it will never pass for NaN, and in case of NaN, the 0...64/255 case will be
  // selected regardless of whether it's saturated before or after the
  // comparisons (always pre-saturating here, but shader translators may choose
  // to saturate later for convenience), as saturation will flush NaN to 0.
  if (gamma >= 96.0f / 255.0f) {
    if (gamma >= 192.0f / 255.0f) {
      scale = 8.0f / 1024.0f;
      offset = -1024.0f;
    } else {
      scale = 4.0f / 1024.0f;
      offset = -256.0f;
    }
  } else {
    if (gamma >= 64.0f / 255.0f) {
      scale = 2.0f / 1024.0f;
      offset = -64.0f;
    } else {
      scale = 1.0f / 1024.0f;
      offset = 0.0f;
      // No `floor` term in this case in the Source Engine, but for the largest
      // value, 1.0, `floor(255.0f * (1.0f / 1024.0f))` is 0 anyway.
    }
  }
  // Though in the Source Engine, the 1/1024 multiplication is done for the
  // truncated part specifically, pre-baking it into the scale is lossless -
  // both 1024 and `scale` are powers of 2.
  float linear = gamma * ((255.0f * 1024.0f) * scale) + offset;
  // For consistency with linear to gamma, and because it's more logical here
  // (0 rather than 1 at -epsilon), using `trunc` instead of `floor`.
  linear += std::trunc(linear * scale);
  linear *= 1.0f / 1023.0f;
  // Clamping is not necessary (1 * (255 * 8) - 1024 + 7 is exactly 1023).
  return linear;
}

float LinearToPWLGamma(float linear) {
  linear = xe::saturate(linear);
  float scale, offset;
  // While the compiled code uses `vcmpgtfp constant, value` comparison
  // (constant > value, or value < constant), it's preferable to use `value >=
  // constant` condition for the higher pieces, as it will never pass for NaN,
  // and in case of NaN, the 0...64/1023 case will be selected regardless of
  // whether it's saturated before or after the comparisons (always
  // pre-saturating here, but shader translators may choose to saturate later
  // for convenience), as saturation will flush NaN to 0.
  if (linear >= 128.0f / 1023.0f) {
    if (linear >= 512.0f / 1023.0f) {
      scale = 1023.0f / 8.0f;
      offset = 128.0f / 255.0f;
    } else {
      scale = 1023.0f / 4.0f;
      offset = 64.0f / 255.0f;
    }
  } else {
    if (linear >= 64.0f / 1023.0f) {
      scale = 1023.0f / 2.0f;
      offset = 32.0f / 255.0f;
    } else {
      scale = 1023.0f;
      offset = 0.0f;
    }
  }
  // The truncation isn't in X360LinearToGamma in the Source Engine, but is
  // there in Direct3D 9 disassembly (the `vrfiz` instructions).
  // It also prevents conversion of 1.0 to 1.0034313725490196078431372549016
  // that's handled via clamping in the Source Engine.
  // 127.875 (1023 / 8) is truncated to 127, which, after scaling, becomes
  // 127 / 255, and when 128 / 255 is added, the result is 1.
  return std::trunc(linear * scale) * (1.0f / 255.0f) + offset;
}

// https://github.com/Microsoft/DirectXTex/blob/master/DirectXTex/DirectXTexConvert.cpp

float Float7e3To32(uint32_t f10) {
  f10 &= 0x3FF;
  if (!f10) {
    return 0.0f;
  }
  uint32_t mantissa = f10 & 0x7F;
  uint32_t exponent = f10 >> 7;
  if (!exponent) {
    // Normalize the value in the resulting float.
    // do { Exponent--; Mantissa <<= 1; } while ((Mantissa & 0x80) == 0)
    uint32_t mantissa_lzcnt = xe::lzcnt(mantissa) - (32 - 8);
    exponent = uint32_t(1 - int32_t(mantissa_lzcnt));
    mantissa = (mantissa << mantissa_lzcnt) & 0x7F;
  }
  return xe::memory::Reinterpret<float>(
      uint32_t(((exponent + 124) << 23) | (mantissa << 16)));
}

// Based on CFloat24 from d3dref9.dll and the 6e4 code from:
// https://github.com/Microsoft/DirectXTex/blob/master/DirectXTex/DirectXTexConvert.cpp
// 6e4 has a different exponent bias allowing [0,512) values, 20e4 allows [0,2).
XE_NOALIAS
uint32_t Float32To20e4(float f32, bool round_to_nearest_even) noexcept {
  if (!(f32 > 0.0f)) {
    // Positive only, and not -0 or NaN.
    return 0;
  }
  auto f32u32 = xe::memory::Reinterpret<uint32_t>(f32);
  if (f32u32 >= 0x3FFFFFF8) {
    // Saturate.
    return 0xFFFFFF;
  }
  if (f32u32 < 0x38800000) {
    // The number is too small to be represented as a normalized 20e4.
    // Convert it to a denormalized value.
    uint32_t shift = std::min(uint32_t(113 - (f32u32 >> 23)), uint32_t(24));
    f32u32 = (0x800000 | (f32u32 & 0x7FFFFF)) >> shift;
  } else {
    // Rebias the exponent to represent the value as a normalized 20e4.
    f32u32 += 0xC8000000u;
  }
  if (round_to_nearest_even) {
    f32u32 += 3 + ((f32u32 >> 3) & 1);
  }
  return (f32u32 >> 3) & 0xFFFFFF;
}
XE_NOALIAS
float Float20e4To32(uint32_t f24) noexcept {
  f24 &= 0xFFFFFF;
  if (!f24) {
    return 0.0f;
  }
  uint32_t mantissa = f24 & 0xFFFFF;
  uint32_t exponent = f24 >> 20;
  if (!exponent) {
    // Normalize the value in the resulting float.
    // do { Exponent--; Mantissa <<= 1; } while ((Mantissa & 0x100000) == 0)
    uint32_t mantissa_lzcnt = xe::lzcnt(mantissa) - (32 - 21);
    exponent = uint32_t(1 - int32_t(mantissa_lzcnt));
    mantissa = (mantissa << mantissa_lzcnt) & 0xFFFFF;
  }
  return xe::memory::Reinterpret<float>(
      uint32_t(((exponent + 112) << 23) | (mantissa << 3)));
}

// Fitted to every mantissa of each function on two binades on the console.
const ScalarEstimate kRcpEstimate = {
    4,
    14,
    4,
    9,
    9,
    true,
    {
        {33554433, 1048569, 32731, 962}, {32537632, 985983, 29846, 852},
        {31580642, 928836, 27292, 758},  {30678338, 876519, 25019, 676},
        {29826162, 828500, 22993, 605},  {29020050, 784321, 21180, 543},
        {28256364, 743585, 19552, 488},  {27531842, 705941, 18088, 441},
        {26843546, 671085, 16765, 399},  {26188825, 638748, 15569, 362},
        {25565282, 608695, 14483, 329},  {24970741, 580713, 13496, 299},
        {24403224, 554617, 12597, 273},  {23860930, 530241, 11776, 250},
        {23342214, 507438, 11025, 229},  {22845571, 486073, 10336, 210},
        {22369622, 466032, 9704, 194},   {21913099, 447205, 9122, 178},
        {21474837, 429495, 8585, 165},   {21053762, 412818, 8090, 152},
        {20648882, 397092, 7633, 141},   {20259280, 382249, 7210, 131},
        {19884108, 368223, 6816, 121},   {19522579, 354955, 6451, 113},
        {19173962, 342390, 6111, 105},   {18837576, 330482, 5796, 98},
        {18512791, 319185, 5501, 91},    {18199014, 308457, 5226, 85},
        {17895698, 298261, 4969, 80},    {17602325, 288561, 4730, 75},
        {17318417, 279328, 4504, 70},    {17043522, 270530, 4292, 66},
    },
};
const ScalarEstimate kRsqEstimate = {
    4,
    15,
    3,
    8,
    11,
    true,
    {
        {33554433, 1048548, 48997, 2300}, {32552581, 957406, 42120, 1872},
        {31635422, 878744, 36523, 1541},  {30791659, 810293, 31913, 1281},
        {30011997, 750289, 28078, 1075},  {29288710, 697341, 24858, 910},
        {28615316, 650341, 22133, 776},   {27986331, 608392, 19807, 666},
        {27397080, 570767, 17810, 576},   {26843546, 536866, 16085, 501},
        {26322263, 506194, 14583, 437},   {25830214, 478333, 13272, 384},
        {25364767, 452938, 12119, 339},   {24923607, 429714, 11102, 300},
        {24504693, 408409, 10200, 267},   {24106216, 388807, 9399, 239},
        {23726567, 741435, 34646, 1626},  {23018151, 676989, 29784, 1323},
        {22369622, 621366, 25825, 1089},  {21772991, 572963, 22566, 906},
        {21221687, 530534, 19854, 760},   {20710246, 493095, 17577, 643},
        {20234084, 459859, 15651, 549},   {19789325, 430197, 14006, 471},
        {19372661, 403593, 12594, 407},   {18981254, 379622, 11373, 354},
        {18612651, 357933, 10312, 309},   {18264720, 338233, 9384, 272},
        {17935599, 320275, 8569, 240},    {17623652, 303854, 7850, 212},
        {17327435, 288788, 7212, 189},    {17045669, 274928, 6646, 169},
    },
};
const ScalarEstimate kSqrtEstimate = {
    5,
    14,
    4,
    10,
    9,
    false,
    {
        {33554432, 524286, 16360, 475}, {34587116, 508633, 14940, 410},
        {35589849, 494303, 13714, 357}, {36565094, 481119, 12647, 313},
        {37514995, 468936, 11712, 275}, {38441431, 457636, 10886, 245},
        {39346059, 447114, 10153, 219}, {40230350, 437286, 9498, 196},
        {41095618, 428079, 8911, 177},  {41943040, 419430, 8382, 160},
        {42773675, 411285, 7904, 146},  {43588485, 403597, 7469, 133},
        {44388341, 396325, 7072, 121},  {45174036, 389432, 6710, 111},
        {45946298, 382886, 6377, 103},  {46705792, 376660, 6071, 95},
        {47453132, 741453, 23137, 671}, {48913569, 719315, 21129, 579},
        {50331648, 699049, 19395, 504}, {51710852, 680405, 17886, 442},
        {53054215, 663177, 16563, 390}, {54364393, 647195, 15395, 346},
        {55643730, 632315, 14359, 309}, {56894307, 618416, 13433, 278},
        {58117981, 605395, 12603, 250}, {59316416, 593164, 11855, 226},
        {60491112, 581645, 11178, 206}, {61643427, 570773, 10563, 187},
        {62774594, 560487, 10004, 171}, {63885735, 550739, 9490, 157},
        {64977878, 541482, 9020, 145},  {66051965, 532677, 8587, 134},
    },
};
const ScalarEstimate kExpEstimate = {
    5,
    13,
    6,
    12,
    6,
    false,
    {
        {33554432, 363410, 7872, 58},   {34289178, 371367, 8045, 60},
        {35040013, 379499, 8221, 61},   {35807290, 387809, 8401, 62},
        {36591367, 396301, 8585, 64},   {37392614, 404979, 8773, 65},
        {38211405, 413847, 8965, 66},   {39048126, 422909, 9161, 68},
        {39903169, 432169, 9362, 69},   {40776934, 441633, 9567, 71},
        {41669833, 451303, 9776, 72},   {42582284, 461185, 9990, 74},
        {43514714, 471284, 10209, 76},  {44467562, 481604, 10432, 77},
        {45441275, 492149, 10661, 79},  {46436310, 502926, 10894, 81},
        {47453132, 513939, 11133, 82},  {48492221, 525192, 11377, 84},
        {49554062, 536693, 11626, 85},  {50639155, 548445, 11880, 87},
        {51748008, 560454, 12140, 90},  {52881142, 572726, 12406, 92},
        {54039088, 585267, 12678, 94},  {55222390, 598083, 12955, 96},
        {56431603, 611179, 13239, 98},  {57667294, 624562, 13529, 100},
        {58930043, 638238, 13825, 102}, {60220443, 652214, 14128, 104},
        {61539099, 666496, 14437, 106}, {62886630, 681090, 14753, 109},
        {64263668, 696004, 15076, 111}, {65670859, 711244, 15406, 113},
    },
};
const ScalarEstimate kLogEstimate = {
    4,
    14,
    5,
    10,
    8,
    false,
    {
        {0, 756386, 23623, 470},        {1489619, 733466, 22213, 430},
        {2934765, 711894, 20926, 394},  {4338018, 691554, 19748, 361},
        {5701736, 672344, 18666, 332},  {7028088, 654173, 17671, 307},
        {8319066, 636958, 16754, 283},  {9576509, 620626, 15906, 262},
        {10802114, 605110, 15121, 243}, {11997454, 590351, 14392, 226},
        {13163988, 576295, 13715, 211}, {14303071, 562893, 13085, 197},
        {15415967, 550100, 12497, 184}, {16503851, 537876, 11948, 172},
        {17567824, 526183, 11434, 161}, {18608913, 514988, 10953, 151},
        {19628084, 504259, 10501, 142}, {20626239, 493967, 10078, 133},
        {21604228, 484088, 9678, 126},  {22562850, 474596, 9303, 118},
        {23502856, 465469, 8949, 111},  {24424957, 456686, 8614, 105},
        {25329821, 448230, 8297, 100},  {26218081, 440080, 7998, 95},
        {27090335, 432222, 7715, 90},   {27947151, 424639, 7447, 85},
        {28789064, 417318, 7192, 81},   {29616586, 410245, 6951, 77},
        {30430198, 403407, 6721, 73},   {31230362, 396794, 6502, 70},
        {32017515, 390393, 6295, 66},   {32792072, 384197, 6096, 64},
    },
};

// Drops a bit, ORing it into the new lowest bit.
static uint32_t RoundToOdd1(uint32_t value) {
  return (value >> 1) | (value & 1);
}

// Shifts right, appending a bit set if any bit shifted out was.
static uint32_t ShiftRightSticky(uint32_t value, uint32_t shift) {
  return ((value >> shift) << 1) |
         uint32_t((value & ((uint32_t(1) << shift) - 1)) != 0);
}

namespace {
// The square, its coarser copy and the cubic coefficient for an offset within a
// segment. Every intermediate here and in the functions below fits in 32 bits,
// the same steps as the shader translators.
struct EstimateTerms {
  uint32_t square;
  uint32_t square_coarse;
  uint32_t cubic;
};
EstimateTerms GetEstimateTerms(const ScalarEstimate& estimate,
                               const uint32_t* coefficients, uint32_t offset) {
  EstimateTerms terms;
  uint32_t square_input = ShiftRightSticky(offset, estimate.square_input_shift);
  terms.square =
      RoundToOdd1((square_input * square_input) >> estimate.square_shift);
  terms.square_coarse =
      RoundToOdd1(terms.square >> estimate.square_coarse_shift)
      << (estimate.square_coarse_shift + 1);
  uint32_t cubic =
      coefficients[3] * ShiftRightSticky(offset, estimate.block_shift);
  if (estimate.cubic_rounds_up) {
    cubic += (uint32_t(1) << estimate.cubic_shift) - 1;
  }
  terms.cubic = RoundToOdd1(cubic >> estimate.cubic_shift);
  return terms;
}
}  // namespace

// The 24-bit mantissa of RCP or RSQ in [2^23, 2^24] for a result in [0.5, 1].
static uint32_t ReciprocalMantissa(const ScalarEstimate& estimate,
                                   uint32_t offset_bits, uint32_t segment,
                                   uint32_t offset) {
  const uint32_t* coefficients = estimate.coefficients[segment];
  EstimateTerms terms = GetEstimateTerms(estimate, coefficients, offset);
  uint32_t higher_order =
      coefficients[0] +
      ((coefficients[2] * terms.square - terms.cubic * terms.square_coarse) >>
       (offset_bits - 3));
  // floor(-C1 * offset / 2^offset_bits), the offset split to stay in 32 bits.
  uint32_t linear = (coefficients[1] * (offset >> 12) +
                     ((coefficients[1] * (offset & 0xFFF)) >> 12)) >>
                    (offset_bits - 12);
  // The linear term is subtracted in ones' complement.
  return (higher_order - (linear + 1)) >> 1;
}

float Rcp(float value) {
  uint32_t bits = xe::memory::Reinterpret<uint32_t>(value);
  uint32_t sign = bits & UINT32_C(0x80000000);
  uint32_t exponent = (bits >> 23) & 0xFF;
  uint32_t result;
  if ((bits & UINT32_C(0x7FFFFFFF)) > UINT32_C(0x7F800000)) {
    result = bits;
  } else if (!exponent) {
    // Denormals are zero.
    result = sign | UINT32_C(0x7F800000);
  } else if (exponent == 0xFF) {
    result = sign;
  } else {
    int32_t magnitude =
        (int32_t(252) - int32_t(exponent)) * (1 << 23) +
        int32_t(ReciprocalMantissa(kRcpEstimate, 18, (bits >> 18) & 0x1F,
                                   bits & 0x3FFFF));
    // Results below the normal range are +0 whatever the sign.
    result = magnitude >= (1 << 23) ? sign | uint32_t(magnitude) : 0;
  }
  return xe::memory::Reinterpret<float>(result);
}

float Rsq(float value) {
  uint32_t bits = xe::memory::Reinterpret<uint32_t>(value);
  uint32_t sign = bits & UINT32_C(0x80000000);
  uint32_t exponent = (bits >> 23) & 0xFF;
  uint32_t result;
  if ((bits & UINT32_C(0x7FFFFFFF)) > UINT32_C(0x7F800000)) {
    result = bits;
  } else if (!exponent) {
    // Denormals are zero.
    result = sign | UINT32_C(0x7F800000);
  } else if (sign) {
    result = UINT32_C(0xFFC00000);
  } else if (exponent == 0xFF) {
    result = 0;
  } else {
    // Segments 16-31 are for an odd unbiased exponent, an even biased one.
    result = ((189 - ((exponent + 1) >> 1)) << 23) +
             ReciprocalMantissa(kRsqEstimate, 19, ((bits >> 19) & 0x1F) ^ 0x10,
                                bits & 0x7FFFF);
  }
  return xe::memory::Reinterpret<float>(result);
}

float Sqrt(float value) {
  uint32_t bits = xe::memory::Reinterpret<uint32_t>(value);
  uint32_t exponent = (bits >> 23) & 0xFF;
  uint32_t result;
  if ((bits & UINT32_C(0x7FFFFFFF)) > UINT32_C(0x7F800000)) {
    result = bits;
  } else if (!exponent) {
    // Denormals are zero.
    result = bits & UINT32_C(0x80000000);
  } else if (bits & UINT32_C(0x80000000)) {
    result = UINT32_C(0xFFC00000);
  } else if (exponent == 0xFF) {
    result = bits;
  } else {
    // Segments 16-31 are for an odd unbiased exponent, an even biased one.
    const uint32_t* coefficients =
        kSqrtEstimate.coefficients[((bits >> 19) & 0x1F) ^ 0x10];
    uint32_t offset = bits & 0x7FFFF;
    EstimateTerms terms = GetEstimateTerms(kSqrtEstimate, coefficients, offset);
    uint32_t higher_order =
        coefficients[0] - ((coefficients[2] * terms.square -
                            terms.cubic * terms.square_coarse + 0x7FFF) >>
                           15);
    uint32_t linear = (coefficients[1] * (offset >> 12) +
                       ((coefficients[1] * (offset & 0xFFF)) >> 12)) >>
                      6;
    // The mantissa of a result in [1, 2).
    result = (((exponent + 127) >> 1) << 23) |
             (((linear + higher_order) >> 2) & 0x7FFFFF);
  }
  return xe::memory::Reinterpret<float>(result);
}

// The mantissa of 2^(fraction / 2^23) for a result in [1, 2).
static uint32_t ExpMantissa(uint32_t fraction) {
  const uint32_t* coefficients = kExpEstimate.coefficients[fraction >> 18];
  uint32_t offset = fraction & 0x3FFFF;
  EstimateTerms terms = GetEstimateTerms(kExpEstimate, coefficients, offset);
  uint32_t higher_order =
      coefficients[0] +
      ((coefficients[2] * terms.square + terms.cubic * terms.square_coarse) >>
       14);
  // floor((C1 * offset + higher_order * 2^17) / 2^19) in 32 bits.
  uint32_t linear_low =
      coefficients[1] * (offset & 0x1FF) + ((higher_order & 3) << 17);
  uint32_t linear_high = coefficients[1] * (offset >> 9) + (linear_low >> 9);
  return ((higher_order >> 2) + (linear_high >> 10)) & 0x7FFFFF;
}

float Exp(float value) {
  uint32_t bits = xe::memory::Reinterpret<uint32_t>(value);
  bool negative = (bits & UINT32_C(0x80000000)) != 0;
  uint32_t exponent = (bits >> 23) & 0xFF;
  uint32_t result;
  if ((bits & UINT32_C(0x7FFFFFFF)) > UINT32_C(0x7F800000)) {
    result = bits;
  } else if (!exponent) {
    // Denormals are zero.
    result = UINT32_C(0x3F800000);
  } else if (exponent >= 127 + 7) {
    result = negative ? 0 : UINT32_C(0x7F800000);
  } else {
    // |x| truncated to fixed point with 23 fraction bits.
    uint32_t significand = (bits & 0x7FFFFF) | 0x800000;
    uint32_t fixed = exponent >= 127       ? significand << (exponent - 127)
                     : 127 - exponent < 24 ? significand >> (127 - exponent)
                                           : 0;
    uint32_t integer = fixed >> 23;
    uint32_t fraction = fixed & 0x7FFFFF;
    if (!negative) {
      result = ((127 + integer) << 23) | ExpMantissa(fraction);
    } else if (!fraction) {
      result = integer < 127 ? (127 - integer) << 23 : 0;
    } else if (integer >= 126) {
      // Results below the normal range are +0.
      result = 0;
    } else {
      // 2^-(i + f) = 2^-(i + 1) * 2^(1 - f), with 1 - f in ones' complement.
      result = ((126 - integer) << 23) | ExpMantissa(0x7FFFFF - fraction);
    }
  }
  return xe::memory::Reinterpret<float>(result);
}

float Log(float value) {
  uint32_t bits = xe::memory::Reinterpret<uint32_t>(value);
  uint32_t exponent = (bits >> 23) & 0xFF;
  uint32_t result;
  if ((bits & UINT32_C(0x7FFFFFFF)) > UINT32_C(0x7F800000)) {
    result = bits;
  } else if (!exponent) {
    // Denormals are zero, and the sign of a zero doesn't matter.
    result = UINT32_C(0xFF800000);
  } else if (bits & UINT32_C(0x80000000)) {
    result = UINT32_C(0xFFC00000);
  } else if (exponent == 0xFF) {
    result = bits;
  } else {
    const uint32_t* coefficients =
        kLogEstimate.coefficients[(bits >> 18) & 0x1F];
    uint32_t offset = bits & 0x3FFFF;
    EstimateTerms terms = GetEstimateTerms(kLogEstimate, coefficients, offset);
    uint32_t linear = (coefficients[1] * (offset >> 9) +
                       ((coefficients[1] * (offset & 0x1FF)) >> 9)) >>
                      8;
    // log2 of the mantissa with 25 fraction bits.
    uint32_t mantissa_log = coefficients[0] + linear -
                            ((coefficients[2] * terms.square -
                              terms.cubic * terms.square_coarse + 0x7FFF) >>
                             15);
    // The exponent added, negative sums negated in ones' complement.
    uint32_t magnitude;
    uint32_t sign;
    if (exponent >= 127) {
      magnitude = ((exponent - 127) << 25) | mantissa_log;
      sign = 0;
    } else {
      magnitude = ((126 - exponent) << 25) | (mantissa_log ^ 0x1FFFFFF);
      sign = UINT32_C(0x80000000);
    }
    if (!magnitude) {
      result = 0;
    } else {
      // Normalized with the low bits truncated.
      uint32_t msb = 31 - xe::lzcnt(magnitude);
      uint32_t significand =
          msb >= 23 ? magnitude >> (msb - 23) : magnitude << (23 - msb);
      result = sign | ((msb + 102) << 23) | (significand & 0x7FFFFF);
    }
  }
  return xe::memory::Reinterpret<float>(result);
}

const char* GetColorRenderTargetFormatName(ColorRenderTargetFormat format) {
  switch (format) {
    case ColorRenderTargetFormat::k_8_8_8_8:
      return "k_8_8_8_8";
    case ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      return "k_8_8_8_8_GAMMA";
    case ColorRenderTargetFormat::k_2_10_10_10:
      return "k_2_10_10_10";
    case ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
      return "k_2_10_10_10_FLOAT";
    case ColorRenderTargetFormat::k_16_16:
      return "k_16_16";
    case ColorRenderTargetFormat::k_16_16_16_16:
      return "k_16_16_16_16";
    case ColorRenderTargetFormat::k_16_16_FLOAT:
      return "k_16_16_FLOAT";
    case ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return "k_16_16_16_16_FLOAT";
    case ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      return "k_2_10_10_10_AS_10_10_10_10";
    case ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
      return "k_2_10_10_10_FLOAT_AS_16_16_16_16";
    case ColorRenderTargetFormat::k_32_FLOAT:
      return "k_32_FLOAT";
    case ColorRenderTargetFormat::k_32_32_FLOAT:
      return "k_32_32_FLOAT";
    default:
      return "kUnknown";
  }
}

const char* GetDepthRenderTargetFormatName(DepthRenderTargetFormat format) {
  switch (format) {
    case DepthRenderTargetFormat::kD24S8:
      return "kD24S8";
    case DepthRenderTargetFormat::kD24FS8:
      return "kD24FS8";
    default:
      return "kUnknown";
  }
}
static const char* const g_endian_names[] = {"none", "8 in 16", "8 in 32",
                                             "16 in 32"};

const char* GetEndianEnglishDescription(xenos::Endian endian) {
  return g_endian_names[static_cast<uint32_t>(endian)];
}
static const char* const g_primtype_human_names[] = {"none",
                                                     "point list",
                                                     "line list",
                                                     "line strip",
                                                     "triangle list",
                                                     "triangle fan",
                                                     "triangle strip",
                                                     "triangle with flags",
                                                     "rectangle list",
                                                     "unused1",
                                                     "unused2",
                                                     "unused3",
                                                     "line loop",
                                                     "quad list",
                                                     "quad strip",
                                                     "polygon",
                                                     "2D copy rect list v0",
                                                     "2D copy rect list v1",
                                                     "2D copy rect list v2",
                                                     "2D copy rect list v3",
                                                     "2D fillrect list",
                                                     "2D line strip",
                                                     "2D triangle strip"};

const char* GetPrimitiveTypeEnglishDescription(xenos::PrimitiveType prim_type) {
  return g_primtype_human_names[static_cast<uint32_t>(prim_type)];
}
}  // namespace xenos
}  // namespace gpu
}  // namespace xe
