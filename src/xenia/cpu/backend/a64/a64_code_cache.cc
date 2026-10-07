/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/a64/a64_code_cache.h"

#include <cstdint>

#include "xenia/base/platform.h"
#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
#endif

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

void A64CodeCache::FillCode(void* write_address, size_t size) {
  // 4-byte aligned. Not brk #0: A64Backend claims that as a breakpoint.
  constexpr uint32_t kBrkFill = 0xD43E0020;  // brk #0xF001
  auto* p = reinterpret_cast<uint32_t*>(write_address);
  auto* end =
      reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(write_address) + size);
  for (; p < end; ++p) {
    *p = kBrkFill;
  }
}

void A64CodeCache::FlushCodeRange(void* address, size_t size) {
#if XE_PLATFORM_WIN32
  FlushInstructionCache(GetCurrentProcess(), address, size);
#else
  __builtin___clear_cache(
      reinterpret_cast<char*>(address),
      reinterpret_cast<char*>(static_cast<uint8_t*>(address) + size));
#endif
}

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe
