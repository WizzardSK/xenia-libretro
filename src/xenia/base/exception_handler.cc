/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/exception_handler.h"

#include <cstddef>

#include "xenia/base/platform.h"

#if XE_ARCH_AMD64
#include <xmmintrin.h>
#elif XE_ARCH_ARM64 && XE_COMPILER_MSVC
#include <intrin.h>
#endif

namespace xe {

void SetHostDefaultFpControl() {
#if XE_ARCH_AMD64
  _mm_setcsr(0x1F80);
#elif XE_ARCH_ARM64 && XE_COMPILER_MSVC
  _WriteStatusReg(ARM64_FPCR, 0);
#elif XE_ARCH_ARM64
  asm volatile("msr fpcr, %0" ::"r"(uint64_t(0)) : "memory");
#endif
}

#if XE_ARCH_AMD64 && !XE_PLATFORM_WIN32
static_assert(offsetof(HostThreadContext, rip) == 0);
static_assert(offsetof(HostThreadContext, eflags) == 8);
static_assert(offsetof(HostThreadContext, int_registers) == 16);
static_assert(offsetof(HostThreadContext, xmm_registers) == 144);

// The resume switches to the interrupted stack with registers still to load,
// so a signal handler that runs without SA_ONSTACK would put its frame on top
// of a context held there. Off the stack it stays out of reach.
static thread_local HostThreadContext resume_context;

XE_NOINLINE void ResumeHostContext(const HostThreadContext* context) {
  resume_context = *context;
  // rax holds the context until the stack carries the last two steps. The
  // stack pointer has to go back before the first push, so the flags land
  // below the resumed frame rather than in the caller's.
  __asm__ volatile(
      "vmovups 144(%%rax), %%xmm0\n"
      "vmovups 160(%%rax), %%xmm1\n"
      "vmovups 176(%%rax), %%xmm2\n"
      "vmovups 192(%%rax), %%xmm3\n"
      "vmovups 208(%%rax), %%xmm4\n"
      "vmovups 224(%%rax), %%xmm5\n"
      "vmovups 240(%%rax), %%xmm6\n"
      "vmovups 256(%%rax), %%xmm7\n"
      "vmovups 272(%%rax), %%xmm8\n"
      "vmovups 288(%%rax), %%xmm9\n"
      "vmovups 304(%%rax), %%xmm10\n"
      "vmovups 320(%%rax), %%xmm11\n"
      "vmovups 336(%%rax), %%xmm12\n"
      "vmovups 352(%%rax), %%xmm13\n"
      "vmovups 368(%%rax), %%xmm14\n"
      "vmovups 384(%%rax), %%xmm15\n"
      "movq 48(%%rax), %%rsp\n"
      "movl 8(%%rax), %%ecx\n"
      "pushq %%rcx\n"
      "popfq\n"
      "movq 24(%%rax), %%rcx\n"
      "movq 32(%%rax), %%rdx\n"
      "movq 40(%%rax), %%rbx\n"
      "movq 56(%%rax), %%rbp\n"
      "movq 64(%%rax), %%rsi\n"
      "movq 72(%%rax), %%rdi\n"
      "movq 80(%%rax), %%r8\n"
      "movq 88(%%rax), %%r9\n"
      "movq 96(%%rax), %%r10\n"
      "movq 104(%%rax), %%r11\n"
      "movq 112(%%rax), %%r12\n"
      "movq 120(%%rax), %%r13\n"
      "movq 128(%%rax), %%r14\n"
      "movq 136(%%rax), %%r15\n"
      "pushq (%%rax)\n"
      "movq 16(%%rax), %%rax\n"
      "ret\n"
      :
      : "a"(&resume_context)
      : "memory");
  __builtin_unreachable();
}
#endif  // XE_ARCH_AMD64 && !XE_PLATFORM_WIN32

// Based on VIXL Instruction::IsLoad and IsStore.
// https://github.com/Linaro/vixl/blob/d48909dd0ac62197edb75d26ed50927e4384a199/src/aarch64/instructions-aarch64.cc#L484
//
// Copyright 2015, VIXL authors
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of ARM Limited nor the names of its contributors may be
//     used to endorse or promote products derived from this software without
//     specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
bool IsArm64LoadPrefetchStore(uint32_t instruction, bool& is_store_out) {
  if ((instruction & kArm64LoadLiteralFMask) == kArm64LoadLiteralFixed) {
    // LDR/LDRSW/PRFM (literal) only read.
    is_store_out = false;
    return true;
  }
  if ((instruction & kArm64SystemSysFMask) == kArm64SystemSysFixed) {
    is_store_out =
        (instruction & kArm64CacheOpByVaFMask) != kArm64CacheOpByVaFixed ||
        (instruction & kArm64DcZvaFMask) == kArm64DcZvaFixed;
    return true;
  }
  if ((instruction & kArm64LoadStoreAnyFMask) != kArm64LoadStoreAnyFixed) {
    return false;
  }
  if ((instruction & kArm64LoadStorePairAnyFMask) ==
      kArm64LoadStorePairAnyFixed) {
    is_store_out = !(instruction & kArm64LoadStorePairLoadBit);
    return true;
  }
  if ((instruction & kArm64LoadStoreExclusiveFMask) ==
      kArm64LoadStoreExclusiveFixed) {
    if ((instruction & kArm64CompareAndSwapFMask) ==
        kArm64CompareAndSwapFixed) {
      is_store_out = true;
    } else {
      is_store_out = !(instruction & kArm64LoadStoreExclusiveLoadBit);
    }
    return true;
  }
  if ((instruction & kArm64AtomicMemoryFMask) == kArm64AtomicMemoryFixed) {
    is_store_out = true;
    return true;
  }
  if ((instruction & kArm64NeonLoadStoreStructFMask) ==
      kArm64NeonLoadStoreStructFixed) {
    is_store_out = !(instruction & kArm64NeonLoadStoreStructLoadBit);
    return true;
  }
  switch (Arm64LoadStoreOp(instruction & kArm64LoadStoreMask)) {
    case Arm64LoadStoreOp::kLDRB_w:
    case Arm64LoadStoreOp::kLDRH_w:
    case Arm64LoadStoreOp::kLDR_w:
    case Arm64LoadStoreOp::kLDR_x:
    case Arm64LoadStoreOp::kLDRSB_x:
    case Arm64LoadStoreOp::kLDRSH_x:
    case Arm64LoadStoreOp::kLDRSW_x:
    case Arm64LoadStoreOp::kLDRSB_w:
    case Arm64LoadStoreOp::kLDRSH_w:
    case Arm64LoadStoreOp::kLDR_b:
    case Arm64LoadStoreOp::kLDR_h:
    case Arm64LoadStoreOp::kLDR_s:
    case Arm64LoadStoreOp::kLDR_d:
    case Arm64LoadStoreOp::kLDR_q:
    case Arm64LoadStoreOp::kPRFM:
      is_store_out = false;
      return true;
    case Arm64LoadStoreOp::kSTRB_w:
    case Arm64LoadStoreOp::kSTRH_w:
    case Arm64LoadStoreOp::kSTR_w:
    case Arm64LoadStoreOp::kSTR_x:
    case Arm64LoadStoreOp::kSTR_b:
    case Arm64LoadStoreOp::kSTR_h:
    case Arm64LoadStoreOp::kSTR_s:
    case Arm64LoadStoreOp::kSTR_d:
    case Arm64LoadStoreOp::kSTR_q:
      is_store_out = true;
      return true;
    default:
      return false;
  }
}

}  // namespace xe
