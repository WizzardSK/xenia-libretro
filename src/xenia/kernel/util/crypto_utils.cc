/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cstring>

#include "xenia/kernel/util/crypto_utils.h"

namespace xe {
namespace kernel {
namespace util {

uint8_t xekey_0x19[] = {0xE1, 0xBC, 0x15, 0x9C, 0x73, 0xB1, 0xEA, 0xE9,
                        0xAB, 0x31, 0x70, 0xF3, 0xAD, 0x47, 0xEB, 0xF3};

uint8_t xekey_0x19_devkit[] = {0xDA, 0xB6, 0x9A, 0xD9, 0x8E, 0x28, 0x76, 0x4F,
                               0x97, 0x7E, 0xE2, 0x48, 0x7E, 0x4F, 0x3F, 0x68};

const uint8_t* GetXeKey(uint32_t idx, bool devkit) {
  if (idx != 0x19) {
    return nullptr;
  }

  return devkit ? xekey_0x19_devkit : xekey_0x19;
}

void RC4(const uint8_t* key, uint32_t key_size_in, const uint8_t* data,
         uint32_t data_size, uint8_t* out, uint32_t out_size) {
  uint8_t tmp_key[0x10];
  uint32_t sbox_size;
  uint8_t sbox[0x100];
  uint32_t i;
  uint32_t j;

  // Setup RC4 session...
  std::memcpy(tmp_key, key, 0x10);
  i = j = 0;
  sbox_size = 0x100;
  for (uint32_t x = 0; x < sbox_size; x++) {
    sbox[x] = (uint8_t)x;
  }

  uint32_t idx = 0;
  for (uint32_t x = 0; x < sbox_size; x++) {
    idx = (idx + sbox[x] + key[x % 0x10]) % sbox_size;
    uint8_t temp = sbox[idx];
    sbox[idx] = sbox[x];
    sbox[x] = temp;
  }

  // Crypt data
  for (uint32_t idx = 0; idx < data_size; idx++) {
    i = (i + 1) % sbox_size;
    j = (j + sbox[i]) % sbox_size;
    uint8_t temp = sbox[i];
    sbox[i] = sbox[j];
    sbox[j] = temp;

    uint8_t a = data[idx];
    uint8_t b = sbox[(sbox[i] + sbox[j]) % sbox_size];
    out[idx] = (uint8_t)(a ^ b);
  }
}

}  // namespace util
}  // namespace kernel
}  // namespace xe
