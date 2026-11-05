/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XBOXKRNL_XBOXKRNL_CRYPT_H_
#define XENIA_KERNEL_XBOXKRNL_XBOXKRNL_CRYPT_H_

#include <cstdint>

namespace xe {
namespace kernel {
namespace xboxkrnl {

// HMAC-SHA1 of the inputs under console key |key_num|. False for a key we
// don't have.
bool XeKeysHmacSha(uint32_t key_num, const uint8_t* inp_1, uint32_t inp_1_size,
                   const uint8_t* inp_2, uint32_t inp_2_size,
                   const uint8_t* inp_3, uint32_t inp_3_size, uint8_t* out,
                   uint32_t out_size);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XBOXKRNL_XBOXKRNL_CRYPT_H_
