/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XBOXKRNL_XECRYPT_MD5_H_
#define XENIA_KERNEL_XBOXKRNL_XECRYPT_MD5_H_

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "xenia/base/assert.h"
#include "xenia/base/byte_order.h"

#include "third_party/mbedtls/include/mbedtls/md5.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

struct XECRYPT_MD5_STATE {
  xe::be<uint32_t> count;     // 0x0
  xe::be<uint32_t> state[4];  // 0x4
  uint8_t buffer[64];         // 0x14
};
static_assert_size(XECRYPT_MD5_STATE, 0x54);

inline mbedtls_md5_context LoadMd5(const XECRYPT_MD5_STATE& state) {
  mbedtls_md5_context context;
  mbedtls_md5_init(&context);
  context.MBEDTLS_PRIVATE(total)[0] = state.count;
  std::ranges::copy(state.state, context.MBEDTLS_PRIVATE(state));
  std::ranges::copy(state.buffer, context.MBEDTLS_PRIVATE(buffer));
  return context;
}

inline void StoreMd5(const mbedtls_md5_context& context,
                     XECRYPT_MD5_STATE& state) {
  state.count = context.MBEDTLS_PRIVATE(total)[0];
  std::ranges::copy(context.MBEDTLS_PRIVATE(state), state.state);
  std::ranges::copy(context.MBEDTLS_PRIVATE(buffer), state.buffer);
}

inline void XeCryptMd5Init(XECRYPT_MD5_STATE* state) {
  mbedtls_md5_context context;
  mbedtls_md5_init(&context);
  mbedtls_md5_starts(&context);
  StoreMd5(context, *state);
}

inline void XeCryptMd5Update(XECRYPT_MD5_STATE* state, const uint8_t* input,
                             uint32_t input_size) {
  auto context = LoadMd5(*state);
  mbedtls_md5_update(&context, input, input_size);
  StoreMd5(context, *state);
}

// Pads in place with a 32-bit bit count and keeps the count, so the last block
// compressed stays in the buffer. The state words are left byte swapped so
// they hold the digest.
inline void XeCryptMd5Final(XECRYPT_MD5_STATE* state, uint8_t* out,
                            uint32_t out_size) {
  const uint32_t count = state->count;
  const uint32_t used = count % 64;
  const uint32_t length_offset = (used < 56 ? 56 : 120) - used;
  uint8_t padding[72] = {0x80};
  const xe::le<uint32_t> bit_count = count << 3;
  std::memcpy(padding + length_offset, &bit_count, sizeof(bit_count));

  auto context = LoadMd5(*state);
  mbedtls_md5_update(&context, padding, length_offset);
  // On its own the length completes the block in the buffer instead of being
  // compressed straight from the padding.
  mbedtls_md5_update(&context, padding + length_offset, 8);
  StoreMd5(context, *state);
  state->count = count;
  for (auto& word : state->state) {
    word = xe::byte_swap(uint32_t(word));
  }

  if (out_size) {
    std::memcpy(out, state->state, std::min<uint32_t>(out_size, 16));
  }
}

inline void XeCryptMd5(const uint8_t* input_1, uint32_t input_1_size,
                       const uint8_t* input_2, uint32_t input_2_size,
                       const uint8_t* input_3, uint32_t input_3_size,
                       uint8_t* output, uint32_t output_size) {
  XECRYPT_MD5_STATE state;
  XeCryptMd5Init(&state);
  if (input_1 && input_1_size) {
    XeCryptMd5Update(&state, input_1, input_1_size);
  }
  if (input_2 && input_2_size) {
    XeCryptMd5Update(&state, input_2, input_2_size);
  }
  if (input_3 && input_3_size) {
    XeCryptMd5Update(&state, input_3, input_3_size);
  }
  XeCryptMd5Final(&state, output, output_size);
}

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XBOXKRNL_XECRYPT_MD5_H_
