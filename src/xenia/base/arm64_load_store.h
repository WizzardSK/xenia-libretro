/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_BASE_ARM64_LOAD_STORE_H_
#define XENIA_BASE_ARM64_LOAD_STORE_H_

#include <cstddef>
#include <cstdint>

#include "xenia/base/platform.h"

#if XE_ARCH_ARM64

namespace xe {

class Exception;

// The host memory an access to |address| goes through, valid for |*span| bytes,
// or nullptr if the access faults. |*natively_readable| is whether a native
// read of |address| succeeds.
using Arm64TranslateFunction = uint8_t* (*)(void* context, uint64_t address,
                                            bool is_write, size_t* span,
                                            bool* natively_readable);

enum class Arm64EmulateResult {
  kDone,
  // The translation refused the access. Nothing was written.
  kFault,
  kUnsupported,
};

// Runs the load or store at the exception's pc against memory reached through
// |translate|, updates the registers it writes and moves the pc past it.
// Handles LDR/STR in all addressing modes, LDP/STP, LDNP/STNP, LDAR/STLR,
// LDAPR, the exclusives, CAS, the LSE atomics, DC ZVA and the cache operations
// by VA. On kFault, |fault_address| and |fault_is_write| name the access.
Arm64EmulateResult EmulateArm64LoadStore(Exception* ex,
                                         Arm64TranslateFunction translate,
                                         void* translate_context,
                                         uint64_t* fault_address,
                                         bool* fault_is_write);

}  // namespace xe

#endif  // XE_ARCH_ARM64

#endif  // XENIA_BASE_ARM64_LOAD_STORE_H_
