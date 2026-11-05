/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XBOXKRNL_XECRYPT_DES_H_
#define XENIA_KERNEL_XBOXKRNL_XECRYPT_DES_H_

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "xenia/base/assert.h"

#include "third_party/mbedtls/include/mbedtls/des.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

// Holds the key instead of a key schedule since Mbed TLS builds separate
// encrypt and decrypt schedules.
struct XECRYPT_DES_STATE {
  uint8_t key[MBEDTLS_DES_KEY_SIZE];
  uint8_t unused[0x78];
};
static_assert_size(XECRYPT_DES_STATE, 0x80);

struct XECRYPT_DES3_STATE {
  XECRYPT_DES_STATE des_state[3];
};
static_assert_size(XECRYPT_DES3_STATE, 0x180);

inline void XeCryptDesParity(const uint8_t* input, uint32_t input_size,
                             uint8_t* output) {
  for (uint32_t offset = 0; offset < input_size;
       offset += MBEDTLS_DES_KEY_SIZE) {
    const uint32_t size =
        std::min<uint32_t>(input_size - offset, MBEDTLS_DES_KEY_SIZE);
    uint8_t key[MBEDTLS_DES_KEY_SIZE] = {};
    std::memcpy(key, input + offset, size);
    mbedtls_des_key_set_parity(key);
    std::memcpy(output + offset, key, size);
  }
}

inline void XeCryptDesKey(XECRYPT_DES_STATE* state, const uint8_t* key) {
  std::memcpy(state->key, key, sizeof(state->key));
}

inline void SetDesKey(mbedtls_des_context* context,
                      const XECRYPT_DES_STATE* state, bool encrypt) {
  if (encrypt) {
    mbedtls_des_setkey_enc(context, state->key);
  } else {
    mbedtls_des_setkey_dec(context, state->key);
  }
}

inline void XeCryptDesEcb(const XECRYPT_DES_STATE* state, const uint8_t* input,
                          uint8_t* output, bool encrypt) {
  mbedtls_des_context context;
  mbedtls_des_init(&context);
  SetDesKey(&context, state, encrypt);
  mbedtls_des_crypt_ecb(&context, input, output);
  mbedtls_des_free(&context);
}

// A partial block at the end is left alone.
inline void XeCryptDesCbc(const XECRYPT_DES_STATE* state, const uint8_t* input,
                          uint32_t input_size, uint8_t* output, uint8_t* feed,
                          bool encrypt) {
  mbedtls_des_context context;
  mbedtls_des_init(&context);
  SetDesKey(&context, state, encrypt);
  mbedtls_des_crypt_cbc(&context,
                        encrypt ? MBEDTLS_DES_ENCRYPT : MBEDTLS_DES_DECRYPT,
                        input_size & ~7u, feed, input, output);
  mbedtls_des_free(&context);
}

inline void XeCryptDes3Key(XECRYPT_DES3_STATE* state, const uint8_t* key) {
  for (int i = 0; i < 3; i++) {
    XeCryptDesKey(&state->des_state[i], key + i * MBEDTLS_DES_KEY_SIZE);
  }
}

inline void SetDes3Key(mbedtls_des3_context* context,
                       const XECRYPT_DES3_STATE* state, bool encrypt) {
  uint8_t key[MBEDTLS_DES_KEY_SIZE * 3];
  for (int i = 0; i < 3; i++) {
    std::memcpy(key + i * MBEDTLS_DES_KEY_SIZE, state->des_state[i].key,
                MBEDTLS_DES_KEY_SIZE);
  }
  if (encrypt) {
    mbedtls_des3_set3key_enc(context, key);
  } else {
    mbedtls_des3_set3key_dec(context, key);
  }
}

inline void XeCryptDes3Ecb(const XECRYPT_DES3_STATE* state,
                           const uint8_t* input, uint8_t* output,
                           bool encrypt) {
  mbedtls_des3_context context;
  mbedtls_des3_init(&context);
  SetDes3Key(&context, state, encrypt);
  mbedtls_des3_crypt_ecb(&context, input, output);
  mbedtls_des3_free(&context);
}

// A partial block at the end is left alone.
inline void XeCryptDes3Cbc(const XECRYPT_DES3_STATE* state,
                           const uint8_t* input, uint32_t input_size,
                           uint8_t* output, uint8_t* feed, bool encrypt) {
  mbedtls_des3_context context;
  mbedtls_des3_init(&context);
  SetDes3Key(&context, state, encrypt);
  mbedtls_des3_crypt_cbc(&context,
                         encrypt ? MBEDTLS_DES_ENCRYPT : MBEDTLS_DES_DECRYPT,
                         input_size & ~7u, feed, input, output);
  mbedtls_des3_free(&context);
}

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XBOXKRNL_XECRYPT_DES_H_
