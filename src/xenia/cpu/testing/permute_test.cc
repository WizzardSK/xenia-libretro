/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/testing/util.h"

using namespace xe;
using namespace xe::cpu;
using namespace xe::cpu::hir;
using namespace xe::cpu::testing;
using xe::cpu::ppc::PPCContext;

TEST_CASE("PERMUTE_V128_BY_INT32_CONSTANT", "[instr]") {
  {
    uint32_t mask = MakePermuteMask(0, 0, 0, 1, 0, 2, 0, 3);
    TestFunction([mask](HIRBuilder& b) {
      StoreVR(b, 3,
              b.Permute(b.LoadConstantUint32(mask), LoadVR(b, 4), LoadVR(b, 5),
                        INT32_TYPE));
      b.Return();
    })
        .Run(
            [](PPCContext* ctx) {
              ctx->v[4] = vec128i(0, 1, 2, 3);
              ctx->v[5] = vec128i(4, 5, 6, 7);
            },
            [](PPCContext* ctx) {
              auto result = ctx->v[3];
              REQUIRE(result == vec128i(0, 1, 2, 3));
            });
  }
  {
    uint32_t mask = MakePermuteMask(1, 0, 1, 1, 1, 2, 1, 3);
    TestFunction([mask](HIRBuilder& b) {
      StoreVR(b, 3,
              b.Permute(b.LoadConstantUint32(mask), LoadVR(b, 4), LoadVR(b, 5),
                        INT32_TYPE));
      b.Return();
    })
        .Run(
            [](PPCContext* ctx) {
              ctx->v[4] = vec128i(0, 1, 2, 3);
              ctx->v[5] = vec128i(4, 5, 6, 7);
            },
            [](PPCContext* ctx) {
              auto result = ctx->v[3];
              REQUIRE(result == vec128i(4, 5, 6, 7));
            });
  }
  {
    uint32_t mask = MakePermuteMask(0, 3, 0, 2, 0, 1, 0, 0);
    TestFunction([mask](HIRBuilder& b) {
      StoreVR(b, 3,
              b.Permute(b.LoadConstantUint32(mask), LoadVR(b, 4), LoadVR(b, 5),
                        INT32_TYPE));
      b.Return();
    })
        .Run(
            [](PPCContext* ctx) {
              ctx->v[4] = vec128i(0, 1, 2, 3);
              ctx->v[5] = vec128i(4, 5, 6, 7);
            },
            [](PPCContext* ctx) {
              auto result = ctx->v[3];
              REQUIRE(result == vec128i(3, 2, 1, 0));
            });
  }
  {
    uint32_t mask = MakePermuteMask(1, 3, 1, 2, 1, 1, 1, 0);
    TestFunction([mask](HIRBuilder& b) {
      StoreVR(b, 3,
              b.Permute(b.LoadConstantUint32(mask), LoadVR(b, 4), LoadVR(b, 5),
                        INT32_TYPE));
      b.Return();
    })
        .Run(
            [](PPCContext* ctx) {
              ctx->v[4] = vec128i(0, 1, 2, 3);
              ctx->v[5] = vec128i(4, 5, 6, 7);
            },
            [](PPCContext* ctx) {
              auto result = ctx->v[3];
              REQUIRE(result == vec128i(7, 6, 5, 4));
            });
  }
}

TEST_CASE("PERMUTE_V128_BY_V128", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3,
            b.Permute(LoadVR(b, 3), LoadVR(b, 4), LoadVR(b, 5), INT8_TYPE));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[3] =
            vec128b(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
        ctx->v[4] =
            vec128b(100, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
        ctx->v[5] = vec128b(16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28,
                            29, 30, 31);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result == vec128b(100, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
                                  13, 14, 15));
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[3] = vec128b(16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28,
                            29, 30, 31);
        ctx->v[4] =
            vec128b(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
        ctx->v[5] = vec128b(116, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28,
                            29, 30, 31);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result == vec128b(116, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26,
                                  27, 28, 29, 30, 31));
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[3] =
            vec128b(15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
        ctx->v[4] =
            vec128b(100, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
        ctx->v[5] = vec128b(16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28,
                            29, 30, 31);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result == vec128b(15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3,
                                  2, 1, 100));
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[3] = vec128b(31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19,
                            18, 17, 16);
        ctx->v[4] =
            vec128b(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
        ctx->v[5] = vec128b(16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28,
                            29, 30, 131);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result == vec128b(131, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21,
                                  20, 19, 18, 17, 16));
      });
}

// ConstantPropagationPass folds an int16 permute once the control and both
// tables are constant, and the backend emits it otherwise. The two paths must
// agree. Nothing caught an inverted table select in the fold because every
// other test feeds its operands in through registers, which stay opaque and so
// never reach the folder. Compare the two paths directly rather than asserting
// a hand-derived result, since the tables are held byte swapped within each
// dword and the index swizzle that follows from that is easy to get wrong.
TEST_CASE("PERMUTE_V128_BY_INT16_FOLD_MATCHES_BACKEND", "[instr]") {
  const vec128_t table_a =
      vec128i(0x00010203, 0x04050607, 0x08090A0B, 0x0C0D0E0F);
  const vec128_t table_b =
      vec128i(0x10111213, 0x14151617, 0x18191A1B, 0x1C1D1E1F);
  // The vmrghh and vmrglh controls, the only two that reach the int16 fold.
  const vec128_t controls[] = {
      vec128s(0, 8, 1, 9, 2, 10, 3, 11),
      vec128s(4, 12, 5, 13, 6, 14, 7, 15),
  };

  for (const vec128_t& control : controls) {
    vec128_t from_backend = vec128i(0, 0, 0, 0);
    TestFunction([&control](HIRBuilder& b) {
      StoreVR(b, 3,
              b.Permute(b.LoadConstantVec128(control), LoadVR(b, 4),
                        LoadVR(b, 5), INT16_TYPE));
      b.Return();
    })
        .Run(
            [&](PPCContext* ctx) {
              ctx->v[4] = table_a;
              ctx->v[5] = table_b;
            },
            [&](PPCContext* ctx) { from_backend = ctx->v[3]; });

    vec128_t from_fold = vec128i(0, 0, 0, 0);
    TestFunction([&](HIRBuilder& b) {
      StoreVR(b, 3,
              b.Permute(b.LoadConstantVec128(control),
                        b.LoadConstantVec128(table_a),
                        b.LoadConstantVec128(table_b), INT16_TYPE));
      b.Return();
    })
        .Run([](PPCContext* ctx) {},
             [&](PPCContext* ctx) { from_fold = ctx->v[3]; });

    REQUIRE(from_fold == from_backend);
  }
}

// Same differential for the int8 permute that vperm emits, including indices
// past 15 that select the second table.
TEST_CASE("PERMUTE_V128_BY_INT8_FOLD_MATCHES_BACKEND", "[instr]") {
  const vec128_t table_a =
      vec128i(0x00010203, 0x04050607, 0x08090A0B, 0x0C0D0E0F);
  const vec128_t table_b =
      vec128i(0x10111213, 0x14151617, 0x18191A1B, 0x1C1D1E1F);
  const vec128_t controls[] = {
      vec128b(0, 1, 2, 3, 16, 17, 18, 19, 4, 5, 6, 7, 20, 21, 22, 23),
      vec128b(31, 30, 29, 28, 27, 26, 25, 24, 7, 6, 5, 4, 3, 2, 1, 0),
  };

  for (const vec128_t& control : controls) {
    vec128_t from_backend = vec128i(0, 0, 0, 0);
    TestFunction([&control](HIRBuilder& b) {
      StoreVR(b, 3,
              b.Permute(b.LoadConstantVec128(control), LoadVR(b, 4),
                        LoadVR(b, 5), INT8_TYPE));
      b.Return();
    })
        .Run(
            [&](PPCContext* ctx) {
              ctx->v[4] = table_a;
              ctx->v[5] = table_b;
            },
            [&](PPCContext* ctx) { from_backend = ctx->v[3]; });

    vec128_t from_fold = vec128i(0, 0, 0, 0);
    TestFunction([&](HIRBuilder& b) {
      StoreVR(b, 3,
              b.Permute(b.LoadConstantVec128(control),
                        b.LoadConstantVec128(table_a),
                        b.LoadConstantVec128(table_b), INT8_TYPE));
      b.Return();
    })
        .Run([](PPCContext* ctx) {},
             [&](PPCContext* ctx) { from_fold = ctx->v[3]; });

    REQUIRE(from_fold == from_backend);
  }
}
