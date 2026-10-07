/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/testing/util.h"

#include <cstddef>

#include "xenia/cpu/compiler/passes/control_flow_analysis_pass.h"
#include "xenia/cpu/compiler/passes/dead_cr_store_elimination_pass.h"

using namespace xe::cpu::hir;
using namespace xe::cpu;
using xe::cpu::ppc::PPCContext;

namespace {

constexpr size_t kCr0Eq = offsetof(PPCContext, cr0.cr0_eq);

size_t CountCr0EqStores(HIRBuilder& b) {
  size_t count = 0;
  for (Block* block = b.first_block(); block; block = block->next) {
    for (Instr* i = block->instr_head; i; i = i->next) {
      if (i->GetOpcodeNum() == OPCODE_STORE_CONTEXT &&
          i->src1.offset == kCr0Eq) {
        ++count;
      }
    }
  }
  return count;
}

// cr0.eq is written, then `call` runs, then cr0.eq is written again.
size_t StoresLeft(void (*call)(HIRBuilder& b)) {
  HIRBuilder b;
  b.MakeCurrent();
  b.StoreContext(kCr0Eq, b.LoadConstantInt8(1));
  call(b);
  b.StoreContext(kCr0Eq, b.LoadConstantInt8(0));
  b.Return();
  b.Finalize();
  compiler::passes::ControlFlowAnalysisPass cfa;
  cfa.Run(&b);
  compiler::passes::DeadCRStoreEliminationPass pass;
  pass.Run(&b);
  const size_t left = CountCr0EqStores(b);
  b.RemoveCurrent();
  return left;
}

}  // namespace

TEST_CASE("DEAD_CR_STORE_ACROSS_CALL", "[compiler]") {
  // A guest callee may read cr0, so neither store is dead.
  REQUIRE(StoresLeft([](HIRBuilder& b) { b.Call(nullptr); }) == 2);
  // `sc` lowers to CALL_EXTERN, and the syscall handler saves the whole CR.
  REQUIRE(StoresLeft([](HIRBuilder& b) { b.CallExtern(nullptr); }) == 2);
}
