/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/net/xnet_secure.h"

#include <algorithm>

#include "third_party/mbedtls/include/mbedtls/dhm.h"
#include "third_party/mbedtls/include/mbedtls/platform_util.h"

namespace xe::net::secure {
namespace {

// RFC 2409 section 6.1, with generator 2.
constexpr uint8_t kPrime[] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xC9, 0x0F, 0xDA, 0xA2,
    0x21, 0x68, 0xC2, 0x34, 0xC4, 0xC6, 0x62, 0x8B, 0x80, 0xDC, 0x1C, 0xD1,
    0x29, 0x02, 0x4E, 0x08, 0x8A, 0x67, 0xCC, 0x74, 0x02, 0x0B, 0xBE, 0xA6,
    0x3B, 0x13, 0x9B, 0x22, 0x51, 0x4A, 0x08, 0x79, 0x8E, 0x34, 0x04, 0xDD,
    0xEF, 0x95, 0x19, 0xB3, 0xCD, 0x3A, 0x43, 0x1B, 0x30, 0x2B, 0x0A, 0x6D,
    0xF2, 0x5F, 0x14, 0x37, 0x4F, 0xE1, 0x35, 0x6D, 0x6D, 0x51, 0xC2, 0x45,
    0xE4, 0x85, 0xB5, 0x76, 0x62, 0x5E, 0x7E, 0xC6, 0xF4, 0x4C, 0x42, 0xE9,
    0xA6, 0x3A, 0x36, 0x20, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};
static_assert(sizeof(kPrime) == sizeof(DhValue));

int RandomBytes(void*, unsigned char* output, size_t size) {
  Random({output, size});
  return 0;
}

// Hands mbedtls_dhm_make_public the caller's exponent as its random bytes.
int ExponentBytes(void* exponent, unsigned char* output, size_t size) {
  const auto& bytes = *static_cast<std::span<const uint8_t>*>(exponent);
  if (size != bytes.size()) {
    return MBEDTLS_ERR_DHM_BAD_INPUT_DATA;
  }
  std::copy(bytes.begin(), bytes.end(), output);
  return 0;
}

// Accepts only values in the prime-order subgroup that 2 generates.
bool InSubgroup(const mbedtls_dhm_context& dhm) {
  mbedtls_mpi prime, peer, order, result;
  mbedtls_mpi_init(&prime);
  mbedtls_mpi_init(&peer);
  mbedtls_mpi_init(&order);
  mbedtls_mpi_init(&result);
  const bool in_subgroup =
      !mbedtls_dhm_get_value(&dhm, MBEDTLS_DHM_PARAM_P, &prime) &&
      !mbedtls_dhm_get_value(&dhm, MBEDTLS_DHM_PARAM_GY, &peer) &&
      !mbedtls_mpi_sub_int(&order, &prime, 1) &&
      !mbedtls_mpi_shift_r(&order, 1) &&
      !mbedtls_mpi_exp_mod(&result, &peer, &order, &prime, nullptr) &&
      !mbedtls_mpi_cmp_int(&result, 1);
  mbedtls_mpi_free(&prime);
  mbedtls_mpi_free(&peer);
  mbedtls_mpi_free(&order);
  mbedtls_mpi_free(&result);
  return in_subgroup;
}

}  // namespace

struct DhKeyPair::Context {
  Context() { mbedtls_dhm_init(&dhm); }
  ~Context() { mbedtls_dhm_free(&dhm); }

  mbedtls_dhm_context dhm;
};

void DhKeyPair::ContextDeleter::operator()(Context* context) const {
  delete context;
}

std::optional<DhKeyPair> DhKeyPair::Generate() {
  std::array<uint8_t, 32> exponent;
  Random(exponent);
  auto result = FromPrivate(exponent);
  mbedtls_platform_zeroize(exponent.data(), exponent.size());
  return result;
}

std::optional<DhKeyPair> DhKeyPair::FromPrivate(
    std::span<const uint8_t> big_endian_exponent) {
  if (big_endian_exponent.size() != 32) {
    return std::nullopt;
  }
  DhKeyPair result;
  result.context_.reset(new Context);
  auto& dhm = result.context_->dhm;
  mbedtls_mpi prime, generator;
  mbedtls_mpi_init(&prime);
  mbedtls_mpi_init(&generator);
  bool valid = !mbedtls_mpi_read_binary(&prime, kPrime, sizeof(kPrime)) &&
               !mbedtls_mpi_lset(&generator, 2) &&
               !mbedtls_dhm_set_group(&dhm, &prime, &generator);
  mbedtls_mpi_free(&prime);
  mbedtls_mpi_free(&generator);
  DhValue public_value;
  valid = valid &&
          !mbedtls_dhm_make_public(&dhm, int(big_endian_exponent.size()),
                                   public_value.data(), public_value.size(),
                                   ExponentBytes, &big_endian_exponent);
  if (!valid) {
    return std::nullopt;
  }
  std::reverse_copy(public_value.begin(), public_value.end(),
                    result.public_.begin());
  return result;
}

std::optional<DhValue> DhKeyPair::Shared(const DhValue& peer) const {
  if (!context_) {
    return std::nullopt;
  }
  auto& dhm = context_->dhm;
  DhValue buffer;
  std::reverse_copy(peer.begin(), peer.end(), buffer.begin());
  size_t length = 0;
  if (mbedtls_dhm_read_public(&dhm, buffer.data(), buffer.size()) ||
      !InSubgroup(dhm) ||
      mbedtls_dhm_calc_secret(&dhm, buffer.data(), buffer.size(), &length,
                              RandomBytes, nullptr)) {
    return std::nullopt;
  }
  // The secret comes back big endian without its leading zero bytes.
  DhValue shared{};
  std::reverse_copy(buffer.begin(), buffer.begin() + length, shared.begin());
  mbedtls_platform_zeroize(buffer.data(), buffer.size());
  return shared;
}

}  // namespace xe::net::secure
