/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XBOXKRNL_XECRYPT_AES_H_
#define XENIA_KERNEL_XBOXKRNL_XECRYPT_AES_H_

#include <cstdint>
#include <cstring>

#include "xenia/base/assert.h"

#include "third_party/mbedtls/include/mbedtls/aes.h"

#if defined(MBEDTLS_AESNI_C) || defined(MBEDTLS_AESCE_C) || \
    defined(MBEDTLS_PADLOCK_C)
#error "LoadAes expects the round keys at the start of the context buffer"
#endif

namespace xe {
namespace kernel {
namespace xboxkrnl {

// keytabdec keeps the decryption round keys in encryption round order, with
// InvMixColumns applied to rounds 1 to 9.
struct XECRYPT_AES_STATE {
  uint8_t keytabenc[11][4][4];  // 0x0
  uint8_t keytabdec[11][4][4];  // 0xB0
};
static_assert_size(XECRYPT_AES_STATE, 0x160);

inline void XeCryptAesKey(XECRYPT_AES_STATE* state, const uint8_t* key) {
  mbedtls_aes_context context;
  mbedtls_aes_init(&context);
  mbedtls_aes_setkey_enc(&context, key, 128);
  std::memcpy(state->keytabenc, context.MBEDTLS_PRIVATE(buf),
              sizeof(state->keytabenc));
  mbedtls_aes_setkey_dec(&context, key, 128);
  for (int round = 0; round < 11; round++) {
    std::memcpy(state->keytabdec[round],
                context.MBEDTLS_PRIVATE(buf) + (10 - round) * 4,
                sizeof(state->keytabdec[round]));
  }
  mbedtls_aes_free(&context);
}

inline mbedtls_aes_context LoadAes(const XECRYPT_AES_STATE& state,
                                   bool encrypt) {
  mbedtls_aes_context context;
  mbedtls_aes_init(&context);
  context.MBEDTLS_PRIVATE(nr) = 10;
  context.MBEDTLS_PRIVATE(rk_offset) = 0;
  uint32_t* round_keys = context.MBEDTLS_PRIVATE(buf);
  if (encrypt) {
    std::memcpy(round_keys, state.keytabenc, sizeof(state.keytabenc));
  } else {
    for (int round = 0; round < 11; round++) {
      std::memcpy(round_keys + (10 - round) * 4, state.keytabdec[round],
                  sizeof(state.keytabdec[round]));
    }
  }
  return context;
}

inline void XeCryptAesEcb(const XECRYPT_AES_STATE* state, const uint8_t* input,
                          uint8_t* output, bool encrypt) {
  auto context = LoadAes(*state, encrypt);
  mbedtls_aes_crypt_ecb(&context,
                        encrypt ? MBEDTLS_AES_ENCRYPT : MBEDTLS_AES_DECRYPT,
                        input, output);
  mbedtls_aes_free(&context);
}

// A partial block at the end is left alone.
inline void XeCryptAesCbc(const XECRYPT_AES_STATE* state, const uint8_t* input,
                          uint32_t input_size, uint8_t* output, uint8_t* feed,
                          bool encrypt) {
  auto context = LoadAes(*state, encrypt);
  mbedtls_aes_crypt_cbc(&context,
                        encrypt ? MBEDTLS_AES_ENCRYPT : MBEDTLS_AES_DECRYPT,
                        input_size & ~15u, feed, input, output);
  mbedtls_aes_free(&context);
}

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XBOXKRNL_XECRYPT_AES_H_
