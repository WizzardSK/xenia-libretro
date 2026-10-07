/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cstring>

#include "xenia/cpu/testing/util.h"

using namespace xe;
using namespace xe::cpu;
using namespace xe::cpu::hir;
using namespace xe::cpu::testing;
using xe::cpu::ppc::PPCContext;

namespace {

constexpr uint64_t kQNaN = 0x7FF8000000000000ull;

Value* CastF64(HIRBuilder& b, Value* value) {
  return b.Cast(value, FLOAT64_TYPE);
}
Value* CastI64(HIRBuilder& b, Value* value) {
  return b.Cast(value, INT64_TYPE);
}

double BitsToDouble(uint64_t bits) {
  double value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}
uint64_t DoubleToBits(double value) {
  uint64_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

// The condition the guest hands a select is a 0/1 byte, so build it the way the
// frontend does rather than passing a constant the folders would eat.
Value* ConditionFromGPR(HIRBuilder& b, int reg) {
  return b.IsTrue(LoadGPR(b, reg));
}

}  // namespace

// fsel covers two register arms 3456 ways against hardware. Nothing covers a
// constant arm, because REGISTER_IN reaches the frontend as a load_context.
TEST_CASE("SELECT_F64_constant_true_arm", "[select]") {
  TestFunction test([](HIRBuilder& b) {
    StoreFPR(b, 4,
             b.Select(ConditionFromGPR(b, 3),
                      CastF64(b, b.LoadConstantUint64(kQNaN)), LoadFPR(b, 5)));
    b.Return();
  });
  const uint64_t kValues[] = {0x0000000000000000ull, 0x3FF0000000000000ull,
                              0xBFF0000000000000ull, 0x000FFFFFFFFFFFFFull,
                              0x7FEFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull};
  for (uint64_t value : kValues) {
    test.Run(
        [&](PPCContext* ctx) {
          ctx->r[3] = 1;
          ctx->f[5] = BitsToDouble(value);
        },
        [&](PPCContext* ctx) { REQUIRE(DoubleToBits(ctx->f[4]) == kQNaN); });
    test.Run(
        [&](PPCContext* ctx) {
          ctx->r[3] = 0;
          ctx->f[5] = BitsToDouble(value);
        },
        [&](PPCContext* ctx) { REQUIRE(DoubleToBits(ctx->f[4]) == value); });
  }
}

TEST_CASE("SELECT_F64_constant_false_arm", "[select]") {
  TestFunction test([](HIRBuilder& b) {
    StoreFPR(b, 4,
             b.Select(ConditionFromGPR(b, 3), LoadFPR(b, 5),
                      CastF64(b, b.LoadConstantUint64(kQNaN))));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[3] = 1;
        ctx->f[5] = BitsToDouble(0x3FF0000000000000ull);
      },
      [](PPCContext* ctx) {
        REQUIRE(DoubleToBits(ctx->f[4]) == 0x3FF0000000000000ull);
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[3] = 0;
        ctx->f[5] = BitsToDouble(0x3FF0000000000000ull);
      },
      [](PPCContext* ctx) { REQUIRE(DoubleToBits(ctx->f[4]) == kQNaN); });
}

// The condition byte is only ever 0 or 1 from a compare, but a select must not
// care which nonzero it gets.
TEST_CASE("SELECT_F64_nonzero_condition", "[select]") {
  TestFunction test([](HIRBuilder& b) {
    StoreFPR(b, 4,
             b.Select(b.Truncate(LoadGPR(b, 3), INT8_TYPE),
                      CastF64(b, b.LoadConstantUint64(kQNaN)), LoadFPR(b, 5)));
    b.Return();
  });
  for (uint64_t condition : {1ull, 2ull, 0x7Full, 0x80ull, 0xFFull}) {
    test.Run(
        [&](PPCContext* ctx) {
          ctx->r[3] = condition;
          ctx->f[5] = BitsToDouble(0x3FF0000000000000ull);
        },
        [](PPCContext* ctx) { REQUIRE(DoubleToBits(ctx->f[4]) == kQNaN); });
  }
  // A condition whose low byte is zero must still select the false arm, even
  // though the wider register is nonzero.
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[3] = 0x100;
        ctx->f[5] = BitsToDouble(0x3FF0000000000000ull);
      },
      [](PPCContext* ctx) {
        REQUIRE(DoubleToBits(ctx->f[4]) == 0x3FF0000000000000ull);
      });
}

// What 28f38aff actually emitted, and what the integer form replaced it with.
// Both must answer the same for the same condition.
TEST_CASE("SELECT_F64_matches_integer_form", "[select]") {
  TestFunction test([](HIRBuilder& b) {
    Value* condition = ConditionFromGPR(b, 3);
    StoreFPR(b, 4,
             b.Select(condition, CastF64(b, b.LoadConstantUint64(kQNaN)),
                      LoadFPR(b, 5)));
    StoreFPR(b, 6,
             CastF64(b, b.Select(condition, b.LoadConstantUint64(kQNaN),
                                 CastI64(b, LoadFPR(b, 5)))));
    b.Return();
  });
  for (uint64_t condition : {0ull, 1ull}) {
    test.Run(
        [&](PPCContext* ctx) {
          ctx->r[3] = condition;
          ctx->f[5] = BitsToDouble(0x4008000000000000ull);
        },
        [](PPCContext* ctx) {
          REQUIRE(DoubleToBits(ctx->f[4]) == DoubleToBits(ctx->f[6]));
        });
  }
}

TEST_CASE("SELECT_V128_FOLD_MATCHES_BACKEND", "[select]") {
  const vec128_t lhs = vec128i(0x00010203, 0x04050607, 0x08090A0B, 0x0C0D0E0F);
  const vec128_t rhs = vec128i(0x10111213, 0x14151617, 0x18191A1B, 0x1C1D1E1F);
  const vec128_t controls[] = {
      vec128i(0, 0, 0, 0),
      vec128i(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF),
      vec128i(0xFFFFFFFF, 0, 0xFFFFFFFF, 0),
      // Partial masks catch a lane-granular blend.
      vec128i(0x0000FFFF, 0xFF00FF00, 0x0F0F0F0F, 0x80000001),
  };
  for (const vec128_t& control : controls) {
    RequireVectorFoldMatchesBackend(
        {control, lhs, rhs}, [](HIRBuilder& b, const std::vector<Value*>& ops) {
          return b.Select(ops[0], ops[1], ops[2]);
        });
  }
}
