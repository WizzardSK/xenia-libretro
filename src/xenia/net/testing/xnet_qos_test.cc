/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "xenia/net/xnet_secure.h"

#include "third_party/catch/include/catch.hpp"
#include "third_party/mbedtls/include/mbedtls/des.h"

namespace xe::net::secure::test {
namespace {

const Key kBroadcastAuth{9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 1, 2, 3, 4, 5, 6};
const KeyId kId{0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
const Key kKey{16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1};
const Block kNonce{0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7};

Address MakeAddress() {
  Address address{};
  for (size_t i = 0; i < address.size(); ++i) {
    address[i] = uint8_t(0x40 + i);
  }
  return address;
}

bool Same(std::span<const uint8_t> a, std::span<const uint8_t> b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

std::vector<uint8_t> Decrypt(const RegistrationKeys& keys, Block iv,
                             std::span<const uint8_t> input) {
  std::vector<uint8_t> output(input.size());
  mbedtls_des3_context context;
  mbedtls_des3_init(&context);
  mbedtls_des3_set3key_dec(&context, keys.cipher.data());
  mbedtls_des3_crypt_cbc(&context, MBEDTLS_DES_DECRYPT, input.size(), iv.data(),
                         input.data(), output.data());
  mbedtls_des3_free(&context);
  return output;
}

// Recomputes the tag after a test edits a reply's header.
void RetagReply(const RegistrationKeys& keys, std::vector<uint8_t>& wire) {
  auto record = std::span(wire).subspan(4);
  const auto tag =
      HmacSha1(keys.authentication, {record.first(0x1B), record.subspan(0x27)});
  std::copy_n(tag.begin(), 12, record.begin() + 0x1B);
}

QosProbe MakeProbe() {
  QosProbe probe;
  probe.nonce = kNonce;
  probe.target = 0x0102;
  probe.sequence = 3;
  probe.flags = kQosProbeDataRequest;
  probe.id = kId;
  probe.address = MakeAddress();
  return probe;
}

}  // namespace

TEST_CASE("QoS probes encode each field at its offset", "[net][qos]") {
  const auto keys = DeriveRegistration(kBroadcastAuth, kId, kKey);
  const auto probe = MakeProbe();
  const auto wire = EncodeQosProbe(keys, probe);
  REQUIRE(wire.size() == 4 + 0x4E);
  CHECK(Same(std::span(wire).first(4), std::array<uint8_t, 4>{}));
  const auto record = std::span(wire).subspan(4);
  CHECK(record[0] == 0x06);
  CHECK(record[1] == 0x58);
  CHECK(record[2] == 0x4E);
  CHECK(record[3] == 0);
  CHECK(record[4] == 0);
  CHECK(record[5] == 0);
  CHECK(Same(record.subspan(6, 8), kNonce));
  CHECK(record[14] == 0x02);
  CHECK(record[15] == 0x01);
  CHECK(record[16] == 3);
  CHECK(record[17] == kQosProbeDataRequest);
  CHECK(Same(record.subspan(18, 8), kId));
  const auto plain = Decrypt(keys, kNonce, record.subspan(26, 40));
  CHECK(Same(std::span(plain).first(36), probe.address));
  CHECK(Same(std::span(plain).last(4), std::array<uint8_t, 4>{}));
  const auto tag = HmacSha1(keys.authentication, {record.first(0x42)});
  CHECK(Same(record.subspan(0x42, 12), std::span(tag).first(12)));

  const auto header = PeekQos(wire);
  REQUIRE(header);
  CHECK_FALSE(header->reply);
  CHECK(header->nonce == kNonce);
  CHECK(header->target == 0x0102);
  CHECK(header->id == kId);

  const auto decoded = DecodeQosProbe(keys, wire);
  REQUIRE(decoded);
  CHECK(decoded->nonce == probe.nonce);
  CHECK(decoded->target == probe.target);
  CHECK(decoded->sequence == probe.sequence);
  CHECK(decoded->flags == probe.flags);
  CHECK(decoded->id == probe.id);
  CHECK(decoded->address == probe.address);
}

TEST_CASE("QoS probes that fail authentication are dropped", "[net][qos]") {
  const auto keys = DeriveRegistration(kBroadcastAuth, kId, kKey);
  const auto wire = EncodeQosProbe(keys, MakeProbe());
  REQUIRE(DecodeQosProbe(keys, wire));
  for (size_t offset : {4u, 10u, 21u, 30u, 60u, 70u, 81u}) {
    auto tampered = wire;
    tampered[offset] ^= 1;
    CHECK_FALSE(DecodeQosProbe(keys, tampered));
  }
  KeyId other_id = kId;
  other_id[7] ^= 1;
  CHECK_FALSE(
      DecodeQosProbe(DeriveRegistration(kBroadcastAuth, other_id, kKey), wire));
  CHECK_FALSE(DecodeQosProbe(keys, std::span(wire).first(wire.size() - 1)));
  auto spi = wire;
  spi[0] = 1;
  CHECK_FALSE(PeekQos(spi));
  CHECK_FALSE(DecodeQosProbe(keys, spi));
}

TEST_CASE("QoS replies encode each field at its offset", "[net][qos]") {
  const auto keys = DeriveRegistration(kBroadcastAuth, kId, kKey);
  for (size_t size : {0u, 5u, 24u}) {
    QosReply reply;
    reply.nonce = kNonce;
    reply.target = 0x0304;
    reply.sequence = 2;
    reply.flags = kQosReplyData | kQosReplyDisabled;
    reply.delay = 0x11223344;
    for (size_t i = 0; i < size; ++i) {
      reply.data.push_back(uint8_t(0x80 + i));
    }
    const auto wire = EncodeQosReply(keys, reply);
    REQUIRE(wire);
    const size_t padded = (size + 7) & ~size_t(7);
    REQUIRE(wire->size() == 4 + 0x27 + padded);
    CHECK(Same(std::span(*wire).first(4), std::array<uint8_t, 4>{}));
    const auto record = std::span(*wire).subspan(4);
    CHECK(record[0] == 0x07);
    CHECK(record[1] == 0x58);
    CHECK(record[2] == 0x27 + padded);
    CHECK(record[3] == 0);
    CHECK(Same(record.subspan(6, 8), kNonce));
    CHECK(record[14] == 0x04);
    CHECK(record[15] == 0x03);
    CHECK(record[16] == 2);
    CHECK(record[17] == (kQosReplyData | kQosReplyDisabled));
    CHECK(Same(record.subspan(18, 4),
               std::array<uint8_t, 4>{0x44, 0x33, 0x22, 0x11}));
    CHECK(Same(record.subspan(22, 4), std::array<uint8_t, 4>{}));
    CHECK(record[26] == padded - size);
    const auto tag = HmacSha1(keys.authentication,
                              {record.first(0x1B), record.subspan(0x27)});
    CHECK(Same(record.subspan(0x1B, 12), std::span(tag).first(12)));
    const auto plain = Decrypt(keys, kNonce, record.subspan(0x27));
    CHECK(Same(std::span(plain).first(size), reply.data));
    CHECK(std::all_of(plain.begin() + size, plain.end(),
                      [](uint8_t byte) { return !byte; }));

    const auto header = PeekQos(*wire);
    REQUIRE(header);
    CHECK(header->reply);
    CHECK(header->nonce == kNonce);
    CHECK(header->target == 0x0304);

    const auto decoded = DecodeQosReply(keys, *wire);
    REQUIRE(decoded);
    CHECK(decoded->nonce == reply.nonce);
    CHECK(decoded->target == reply.target);
    CHECK(decoded->sequence == reply.sequence);
    CHECK(decoded->flags == reply.flags);
    CHECK(decoded->delay == reply.delay);
    CHECK(decoded->gap == 0);
    CHECK(decoded->data == reply.data);
  }
}

TEST_CASE("QoS replies of a probe pair carry its timing", "[net][qos]") {
  const auto keys = DeriveRegistration(kBroadcastAuth, kId, kKey);
  QosReply reply;
  reply.nonce = kNonce;
  reply.sequence = 1;
  reply.flags = kQosReplySecond | kQosReplyNoData;
  reply.delay = 7;
  reply.gap = 0x0A0B0C0D;
  const auto wire = EncodeQosReply(keys, reply);
  REQUIRE(wire);
  REQUIRE(wire->size() == 4 + 0x27);
  const auto record = std::span(*wire).subspan(4);
  CHECK(Same(record.subspan(22, 4),
             std::array<uint8_t, 4>{0x0D, 0x0C, 0x0B, 0x0A}));
  const auto decoded = DecodeQosReply(keys, *wire);
  REQUIRE(decoded);
  CHECK(decoded->flags == reply.flags);
  CHECK(decoded->delay == 7);
  CHECK(decoded->gap == reply.gap);
  CHECK(decoded->data.empty());

  // Only a reply that says it carries data may.
  reply.data = {1, 2, 3};
  CHECK_FALSE(EncodeQosReply(keys, reply));
}

TEST_CASE("QoS replies that fail checks are dropped", "[net][qos]") {
  const auto keys = DeriveRegistration(kBroadcastAuth, kId, kKey);
  QosReply reply;
  reply.nonce = kNonce;
  reply.flags = kQosReplyData;
  reply.data = {1, 2, 3, 4, 5};
  const auto wire = EncodeQosReply(keys, reply);
  REQUIRE(wire);
  REQUIRE(DecodeQosReply(keys, *wire));
  for (size_t offset : {4u, 12u, 22u, 30u, 35u, 45u}) {
    auto tampered = *wire;
    tampered[offset] ^= 1;
    CHECK_FALSE(DecodeQosReply(keys, tampered));
  }
  // Padding is less than a block.
  auto padding = *wire;
  padding[4 + 26] = 8;
  RetagReply(keys, padding);
  CHECK_FALSE(DecodeQosReply(keys, padding));
  // Without the data flag there is nothing to decrypt.
  auto flags = *wire;
  flags[4 + 17] = 0;
  RetagReply(keys, flags);
  CHECK_FALSE(DecodeQosReply(keys, flags));
  // Ciphertext comes in whole blocks.
  auto partial = *wire;
  partial.pop_back();
  partial[4 + 2] -= 1;
  RetagReply(keys, partial);
  CHECK_FALSE(DecodeQosReply(keys, partial));
  CHECK_FALSE(DecodeQosProbe(keys, *wire));
  CHECK_FALSE(DecodeQosReply(keys, EncodeQosProbe(keys, MakeProbe())));
}

}  // namespace xe::net::secure::test
