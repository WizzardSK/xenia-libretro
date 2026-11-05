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
#include <bit>
#include <cstring>
#include <limits>
#include <random>

// This header has no C++ linkage guard of its own.
extern "C" {
#include "third_party/mbedtls/include/mbedtls/constant_time.h"
}
#include "third_party/mbedtls/include/mbedtls/des.h"
#include "third_party/mbedtls/include/mbedtls/md.h"

namespace xe::net {

void Random(std::span<uint8_t> bytes) {
  std::random_device device;
  for (size_t i = 0; i < bytes.size(); i += sizeof(uint32_t)) {
    const uint32_t value = device();
    std::memcpy(bytes.data() + i, &value,
                std::min(sizeof(value), bytes.size() - i));
  }
}

}  // namespace xe::net

namespace xe::net::secure {
namespace {

uint64_t Read(std::span<const uint8_t> bytes, bool little = false) {
  uint64_t result = 0;
  for (size_t i = 0; i < bytes.size(); ++i) {
    result = (result << 8) | bytes[little ? bytes.size() - 1 - i : i];
  }
  return result;
}

void Write(std::span<uint8_t> bytes, uint64_t value, bool little = false) {
  for (size_t i = 0; i < bytes.size(); ++i) {
    bytes[little ? i : bytes.size() - 1 - i] = static_cast<uint8_t>(value);
    value >>= 8;
  }
}

void OddParity(std::array<uint8_t, 24>& key) {
  for (size_t i = 0; i < key.size(); i += MBEDTLS_DES_KEY_SIZE) {
    mbedtls_des_key_set_parity(key.data() + i);
  }
}

bool Equal(std::span<const uint8_t> a, std::span<const uint8_t> b) {
  return a.size() == b.size() &&
         !mbedtls_ct_memcmp(a.data(), b.data(), a.size());
}

// Triple DES with all 24 key bytes, or single DES with the first 8.
std::vector<uint8_t> Cbc(const std::array<uint8_t, 24>& key, Block iv,
                         std::span<const uint8_t> input, bool encrypt,
                         bool triple) {
  std::vector<uint8_t> output(input.size());
  const int mode = encrypt ? MBEDTLS_DES_ENCRYPT : MBEDTLS_DES_DECRYPT;
  if (triple) {
    mbedtls_des3_context context;
    mbedtls_des3_init(&context);
    if (encrypt) {
      mbedtls_des3_set3key_enc(&context, key.data());
    } else {
      mbedtls_des3_set3key_dec(&context, key.data());
    }
    mbedtls_des3_crypt_cbc(&context, mode, input.size(), iv.data(),
                           input.data(), output.data());
    mbedtls_des3_free(&context);
  } else {
    mbedtls_des_context context;
    mbedtls_des_init(&context);
    if (encrypt) {
      mbedtls_des_setkey_enc(&context, key.data());
    } else {
      mbedtls_des_setkey_dec(&context, key.data());
    }
    mbedtls_des_crypt_cbc(&context, mode, input.size(), iv.data(), input.data(),
                          output.data());
    mbedtls_des_free(&context);
  }
  return output;
}

Sha1Digest Tag(const Key& key, std::span<const uint8_t> descriptor,
               std::span<const uint8_t> ciphertext,
               std::span<const uint8_t> tail, uint32_t sequence) {
  std::vector<uint8_t> message(tail.begin(), tail.end());
  message.resize(tail.size() + 12, 0);
  Write(std::span(message).subspan(tail.size(), 4), sequence, true);
  std::copy(descriptor.begin(), descriptor.end(),
            message.begin() + tail.size() + 4);
  message.resize((tail.size() + 11) & ~size_t(3));
  return HmacSha1(key, {message, ciphertext});
}

std::vector<uint8_t> Ports(uint16_t source, uint16_t destination,
                           uint8_t& encoding) {
  if (source == 1000 && destination == 1000) {
    encoding = 0;
    return {};
  }
  if (source >= 1000 && source <= 1255 && destination >= 1000 &&
      destination <= 1255) {
    encoding = 1;
    return {uint8_t(source - 1000), uint8_t(destination - 1000)};
  }
  encoding = 2;
  std::vector<uint8_t> result(4);
  Write(std::span(result).first(2), source);
  Write(std::span(result).last(2), destination);
  return result;
}

bool ValidSpi(uint32_t spi) { return spi && spi <= 0xFFFFFF; }

// Sequence, acknowledgment, offset, flags and window.
constexpr size_t kTcpHeaderSize = 12;

// The header length must cover the fixed fields and its options come out of
// the encrypted part.
bool ValidTcpOffset(uint8_t offset, size_t encrypted_size) {
  const size_t words = offset >> 4;
  return words >= 5 && (words - 5) * 4 <= encrypted_size;
}

constexpr uint16_t kQosProbeType = 0x0658;
constexpr uint16_t kQosReplyType = 0x0758;
constexpr size_t kQosProbeSize = 78;
constexpr size_t kQosReplyHeaderSize = 39;
constexpr size_t kQosTagSize = 12;
// Records that carry it are dropped.
constexpr uint8_t kQosRejectedFlags = 0x20;

bool ValidExchange(const Exchange& exchange) {
  if ((!SystemLinkKeyId(exchange.id, false) &&
       !SystemLinkKeyId(exchange.id, true)) ||
      !ValidSpi(exchange.initiator_spi) || !(exchange.hops & 0xFF)) {
    return false;
  }
  return exchange.response ? ValidSpi(exchange.responder_spi)
                           : !exchange.responder_spi &&
                                 exchange.responder_challenge == Block{};
}

}  // namespace

Sha1Digest HmacSha1(std::span<const uint8_t> key,
                    std::initializer_list<std::span<const uint8_t>> parts) {
  Sha1Digest digest{};
  mbedtls_md_context_t context;
  mbedtls_md_init(&context);
  mbedtls_md_setup(&context, mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), 1);
  mbedtls_md_hmac_starts(&context, key.data(), key.size());
  for (auto part : parts) {
    mbedtls_md_hmac_update(&context, part.data(), part.size());
  }
  mbedtls_md_hmac_finish(&context, digest.data());
  mbedtls_md_free(&context);
  return digest;
}

std::optional<ProtectionKeys> DeriveBroadcast(
    const Key& executable_key, const PlatformHmac& platform_hmac) {
  if (!platform_hmac) {
    return std::nullopt;
  }
  std::array<uint8_t, 60> stream{};
  std::array<uint8_t, 17> input{};
  std::copy(executable_key.begin(), executable_key.end(), input.begin() + 1);
  for (uint8_t i = 0; i < 3; ++i) {
    input[0] = i;
    Sha1Digest digest{};
    if (!platform_hmac(input, digest)) {
      return std::nullopt;
    }
    std::copy(digest.begin(), digest.end(), stream.begin() + 20 * i);
  }
  ProtectionKeys result;
  std::copy_n(stream.begin(), 16, result.authentication.begin());
  std::copy_n(stream.begin() + 16, 24, result.cipher.begin());
  std::copy_n(stream.begin() + 40, 8, result.iv_seed.begin());
  OddParity(result.cipher);
  return result;
}

RegistrationKeys DeriveRegistration(const Key& broadcast_auth, const KeyId& id,
                                    const Key& key) {
  KeyId counter_id = id;
  auto first = HmacSha1(broadcast_auth, {counter_id, key});
  ++counter_id[0];
  auto second = HmacSha1(broadcast_auth, {counter_id, key});
  RegistrationKeys result;
  std::copy_n(first.begin(), 16, result.authentication.begin());
  std::copy_n(first.begin() + 16, 4, result.cipher.begin());
  std::copy(second.begin(), second.end(), result.cipher.begin() + 4);
  OddParity(result.cipher);
  return result;
}

TrafficKeys DeriveTraffic(const Key& registration_auth, const DhValue& shared,
                          const Block& initiator, const Block& responder) {
  std::array<uint8_t, 100> stream{};
  DhValue value = shared;
  for (uint8_t i = 0; i < 5; ++i) {
    value[0] = uint8_t(shared[0] + i);
    const auto digest =
        HmacSha1(registration_auth, {value, initiator, responder});
    std::copy(digest.begin(), digest.end(), stream.begin() + 20 * i);
  }
  TrafficKeys result;
  std::copy_n(stream.begin(), 16, result.a.authentication.begin());
  std::copy_n(stream.begin() + 16, 24, result.a.cipher.begin());
  std::copy_n(stream.begin() + 40, 16, result.b.authentication.begin());
  std::copy_n(stream.begin() + 56, 24, result.b.cipher.begin());
  std::copy_n(stream.begin() + 80, 8, result.a.iv_seed.begin());
  std::copy_n(stream.begin() + 88, 8, result.b.iv_seed.begin());
  OddParity(result.a.cipher);
  OddParity(result.b.cipher);
  return result;
}

Block PacketIv(const Block& seed, uint32_t sequence) {
  const uint64_t a = Read(std::span(seed).first(4), true) * sequence;
  const uint64_t b = Read(std::span(seed).last(4), true) * sequence;
  Block result;
  Write(std::span(result).first(4), uint32_t(a) ^ uint32_t(b >> 32), true);
  Write(std::span(result).last(4), uint32_t(b) ^ uint32_t(a >> 32), true);
  return result;
}

Metadata MakeMetadata(uint32_t executable_id, uint32_t version, uint16_t major,
                      uint16_t minor, uint16_t build, uint8_t revision) {
  Metadata result{};
  Write(std::span(result).first(4), executable_id, true);
  Write(std::span(result).subspan(4, 4), version, true);
  const uint32_t kernel = (uint32_t(major & 15) << 28) |
                          (uint32_t(minor & 15) << 24) |
                          (uint32_t(build) << 8) | revision;
  Write(std::span(result).last(4), kernel, true);
  return result;
}

std::optional<Metadata> PeekMetadata(std::span<const uint8_t> wire) {
  // The metadata, the block count, the sequence and the tag end the packet.
  constexpr size_t kTrailer = 12 + 1 + 2 + 10;
  if (wire.size() < 4 + kTrailer || Read(wire.subspan(1, 3))) {
    return std::nullopt;
  }
  const uint8_t kind = wire[0] & 15;
  const size_t reserved = (wire[0] >> 5) | ((wire[0] & 0x10) >> 1);
  if (kind < 7 || kind > 9 || reserved > 7) {
    return std::nullopt;
  }
  // Ports that don't fit in the padding come before the metadata.
  const size_t ports = 2 * ((kind - 1) % 3);
  const size_t clear_ports = ports - std::min(reserved, ports);
  const size_t blocks = wire[wire.size() - 13];
  if (wire.size() != 4 + blocks * 8 + clear_ports + kTrailer) {
    return std::nullopt;
  }
  Metadata metadata;
  std::copy_n(wire.end() - kTrailer, metadata.size(), metadata.begin());
  return metadata;
}

uint32_t BroadcastVersionDifference(const Metadata& own,
                                    const Metadata& theirs) {
  auto field = [](const Metadata& metadata, size_t index) {
    return Read(std::span(metadata).subspan(4 * index, 4), true);
  };
  if (field(theirs, 0) != field(own, 0)) {
    return 0;
  }
  uint32_t difference = 0;
  // The version, then the kernel.
  for (size_t index : {1, 2}) {
    if (field(theirs, index) < field(own, index)) {
      difference |= kBroadcastVersionOlder;
    } else if (field(theirs, index) > field(own, index)) {
      difference |= kBroadcastVersionNewer;
    }
  }
  return difference;
}

bool FreshnessNewer(uint64_t incoming, uint64_t previous, bool response) {
  return response ? std::bit_cast<int64_t>(incoming - previous) > 0
                  : std::bit_cast<int64_t>(incoming) >
                        std::bit_cast<int64_t>(previous);
}

bool Encode(const ProtectionKeys& keys, uint32_t spi, uint32_t sequence,
            const Packet& packet, std::vector<uint8_t>& wire) {
  wire.clear();
  const bool discovery = packet.kind == Kind::Discovery;
  if ((discovery ? spi != 0 || sequence > 0xFFFF : !ValidSpi(spi)) ||
      packet.encrypted.size() > kMaxDatagramSize ||
      packet.clear.size() > kMaxDatagramSize) {
    return false;
  }
  uint8_t encoding = 0;
  auto header =
      packet.kind == Kind::Control
          ? std::vector<uint8_t>{}
          : Ports(packet.source_port, packet.destination_port, encoding);
  if (packet.kind == Kind::Tcp) {
    header.resize(header.size() + kTcpHeaderSize);
    auto fields = std::span(header).last(kTcpHeaderSize);
    Write(fields.first(4), packet.tcp.sequence);
    Write(fields.subspan(4, 4), packet.tcp.acknowledgment);
    fields[8] = packet.tcp.offset;
    fields[9] = packet.tcp.flags;
    Write(fields.last(2), packet.tcp.window);
  }
  std::vector<uint8_t> plain = packet.encrypted;
  uint8_t kind;
  if (packet.kind == Kind::Control) {
    if (!packet.clear.empty() ||
        (packet.control_type == 0x0153
             ? !plain.empty()
             : packet.control_type != 0x0053 || plain.size() != 4)) {
      return false;
    }
    plain.insert(plain.begin(), 4, 0);
    Write(std::span(plain).first(2), packet.control_type);
    Write(std::span(plain).subspan(2, 2), plain.size(), true);
    kind = 0;
  } else if (discovery) {
    if (!packet.clear.empty()) {
      return false;
    }
    kind = 7 + encoding;
  } else if (packet.kind == Kind::Tcp) {
    if (!packet.clear.empty() ||
        !ValidTcpOffset(packet.tcp.offset, plain.size())) {
      return false;
    }
    kind = 1 + encoding;
  } else if (packet.kind == Kind::Udp) {
    if (!packet.clear.empty()) {
      return false;
    }
    kind = 4 + encoding;
  } else {
    kind = (plain.empty() ? 10 : packet.clear.empty() ? 13 : 7) + encoding;
  }
  const uint8_t reserved = uint8_t((8 - plain.size() % 8) % 8);
  // Header bytes, the ports and then the rest of a TCP header, fill the cipher
  // padding first and any remainder goes in the clear tail.
  const size_t embedded = std::min<size_t>(reserved, header.size());
  plain.resize(plain.size() + reserved - embedded, 0);
  plain.insert(plain.end(), header.begin(), header.begin() + embedded);
  const bool count = kind >= 7 && kind <= 9;
  if (count && plain.size() > 2040) {
    return false;
  }
  std::array<uint8_t, 4> descriptor{uint8_t(kind | (reserved << 5)),
                                    uint8_t(spi >> 16), uint8_t(spi >> 8),
                                    uint8_t(spi)};
  const uint32_t full_sequence =
      discovery ? sequence | (sequence << 16) : sequence;
  auto ciphertext = Cbc(keys.cipher, PacketIv(keys.iv_seed, full_sequence),
                        plain, true, discovery);
  std::vector<uint8_t> tail(header.begin() + embedded, header.end());
  tail.insert(tail.end(), packet.clear.begin(), packet.clear.end());
  if (discovery) {
    tail.insert(tail.end(), packet.metadata.begin(), packet.metadata.end());
  }
  if (count) {
    tail.push_back(uint8_t(ciphertext.size() / 8));
  }
  if (16 + ciphertext.size() + tail.size() > kMaxDatagramSize) {
    return false;
  }
  const auto tag =
      Tag(keys.authentication, descriptor, ciphertext, tail, full_sequence);
  wire.insert(wire.end(), descriptor.begin(), descriptor.end());
  wire.insert(wire.end(), ciphertext.begin(), ciphertext.end());
  wire.insert(wire.end(), tail.begin(), tail.end());
  wire.push_back(uint8_t(sequence));
  wire.push_back(uint8_t(sequence >> 8));
  wire.insert(wire.end(), tag.begin(), tag.begin() + 10);
  return true;
}

std::optional<Packet> Decode(const ProtectionKeys& keys, uint32_t spi,
                             uint32_t sequence, std::span<const uint8_t> wire,
                             bool discovery) {
  if (wire.size() < 16 || wire.size() > kMaxDatagramSize ||
      (discovery ? spi != 0 || sequence > 0xFFFF : !ValidSpi(spi)) ||
      Read(wire.subspan(1, 3)) != spi ||
      Read(wire.subspan(wire.size() - 12, 2), true) != uint16_t(sequence)) {
    return std::nullopt;
  }
  const uint8_t kind = wire[0] & 15;
  if (discovery && (kind < 7 || kind > 9)) {
    return std::nullopt;
  }
  const size_t reserved = (wire[0] >> 5) | ((wire[0] & 0x10) >> 1);
  // Padding is at most seven bytes.
  if (reserved > 7) {
    return std::nullopt;
  }
  const uint8_t encoding = kind ? (kind - 1) % 3 : 0;
  const bool tcp = kind >= 1 && kind <= 3;
  const size_t header_size = 2 * encoding + (tcp ? kTcpHeaderSize : 0);
  const size_t embedded = std::min(reserved, header_size);
  const size_t clear_header = header_size - embedded;
  const bool count = kind >= 7 && kind <= 9;
  size_t ciphertext_size;
  if (count) {
    if (wire.size() < 17 + clear_header + (discovery ? 12 : 0)) {
      return std::nullopt;
    }
    ciphertext_size = size_t(wire[wire.size() - 13]) * 8;
  } else if (kind >= 10 && kind <= 12) {
    if (reserved) {
      return std::nullopt;
    }
    ciphertext_size = 0;
  } else {
    if (wire.size() < 16 + clear_header) {
      return std::nullopt;
    }
    ciphertext_size = (wire.size() - 16 - clear_header) & ~size_t(7);
  }
  if (ciphertext_size < reserved || ciphertext_size > wire.size() - 16) {
    return std::nullopt;
  }
  const auto ciphertext = wire.subspan(4, ciphertext_size);
  auto tail =
      wire.subspan(4 + ciphertext_size, wire.size() - 16 - ciphertext_size);
  if (tail.size() < clear_header + (count ? 1 : 0) + (discovery ? 12 : 0)) {
    return std::nullopt;
  }
  const uint32_t full_sequence =
      discovery ? sequence | (sequence << 16) : sequence;
  const auto tag =
      Tag(keys.authentication, wire.first(4), ciphertext, tail, full_sequence);
  if (!Equal(std::span(tag).first(10), wire.last(10))) {
    return std::nullopt;
  }
  auto plain = Cbc(keys.cipher, PacketIv(keys.iv_seed, full_sequence),
                   ciphertext, false, discovery);
  const size_t body_size = plain.size() - reserved;
  for (size_t i = body_size; i < plain.size() - embedded; ++i) {
    if (plain[i]) {
      return std::nullopt;
    }
  }
  Packet packet;
  packet.kind = discovery   ? Kind::Discovery
                : kind == 0 ? Kind::Control
                : tcp       ? Kind::Tcp
                : kind <= 6 ? Kind::Udp
                            : Kind::Vdp;
  std::vector<uint8_t> header(plain.end() - embedded, plain.end());
  header.insert(header.end(), tail.begin(), tail.begin() + clear_header);
  if (encoding == 1) {
    packet.source_port = 1000 + header[0];
    packet.destination_port = 1000 + header[1];
  } else if (encoding == 2) {
    packet.source_port = uint16_t(Read(std::span(header).first(2)));
    packet.destination_port = uint16_t(Read(std::span(header).subspan(2, 2)));
  }
  if (tcp) {
    auto fields = std::span(header).last(kTcpHeaderSize);
    packet.tcp.sequence = uint32_t(Read(fields.first(4)));
    packet.tcp.acknowledgment = uint32_t(Read(fields.subspan(4, 4)));
    packet.tcp.offset = fields[8];
    packet.tcp.flags = fields[9];
    packet.tcp.window = uint16_t(Read(fields.last(2)));
    if (!ValidTcpOffset(packet.tcp.offset, body_size)) {
      return std::nullopt;
    }
  }
  packet.encrypted.assign(plain.begin(), plain.begin() + body_size);
  tail = tail.subspan(clear_header);
  if (count) {
    tail = tail.first(tail.size() - 1);
  }
  if (discovery) {
    if (tail.size() != 12) {
      return std::nullopt;
    }
    std::copy(tail.begin(), tail.end(), packet.metadata.begin());
  } else {
    packet.clear.assign(tail.begin(), tail.end());
  }
  if ((kind <= 6 || kind >= 13) && !packet.clear.empty()) {
    return std::nullopt;
  }
  if (kind == 0) {
    if (body_size < 4 ||
        Read(std::span(plain).subspan(2, 2), true) != body_size) {
      return std::nullopt;
    }
    packet.control_type = uint16_t(Read(std::span(plain).first(2)));
    if (packet.control_type == 0x0153
            ? body_size != 4
            : packet.control_type != 0x0053 || body_size != 8) {
      return std::nullopt;
    }
    packet.encrypted.erase(packet.encrypted.begin(),
                           packet.encrypted.begin() + 4);
  }
  return packet;
}

std::optional<Packet> FromGuestVdp(uint16_t source, uint16_t destination,
                                   std::span<const uint8_t> guest) {
  if (guest.size() < 2 || guest.size() > kMaxDatagramSize) {
    return std::nullopt;
  }
  const size_t encrypted = Read(guest.first(2));
  guest = guest.subspan(2);
  if (encrypted > guest.size()) {
    return std::nullopt;
  }
  Packet packet;
  packet.kind = Kind::Vdp;
  packet.source_port = source;
  packet.destination_port = destination;
  packet.encrypted.assign(guest.begin(), guest.begin() + encrypted);
  packet.clear.assign(guest.begin() + encrypted, guest.end());
  return packet;
}

std::vector<uint8_t> ToGuestVdp(const Packet& packet) {
  if (packet.kind != Kind::Vdp || packet.encrypted.size() > 0xFFFF ||
      packet.encrypted.size() + packet.clear.size() + 2 > kMaxDatagramSize) {
    return {};
  }
  std::vector<uint8_t> result(2);
  Write(result, packet.encrypted.size());
  result.insert(result.end(), packet.encrypted.begin(), packet.encrypted.end());
  result.insert(result.end(), packet.clear.begin(), packet.clear.end());
  return result;
}

std::optional<std::array<uint8_t, 256>> EncodeExchange(
    const RegistrationKeys& keys, const Exchange& exchange) {
  if (!ValidExchange(exchange)) {
    return std::nullopt;
  }
  std::array<uint8_t, 256> wire{};
  auto records = std::span(wire).subspan(4);
  Write(records.first(2), exchange.response ? 0x0158 : 0x0058);
  Write(records.subspan(2, 2), 128, true);
  Write(records.subspan(4, 2), exchange.reserved);
  Write(records.subspan(6, 2), exchange.hops, true);
  std::copy(exchange.id.begin(), exchange.id.end(), records.begin() + 8);
  Write(records.subspan(16, 4), exchange.initiator_spi);
  Write(records.subspan(20, 4), exchange.responder_spi);
  std::copy(exchange.initiator_challenge.begin(),
            exchange.initiator_challenge.end(), records.begin() + 24);
  std::copy(exchange.responder_challenge.begin(),
            exchange.responder_challenge.end(), records.begin() + 32);
  Write(records.subspan(40, 8), exchange.freshness, true);
  std::copy(exchange.address_iv.begin(), exchange.address_iv.end(),
            records.begin() + 48);
  std::array<uint8_t, 72> addresses;
  std::copy(exchange.initiator_address.begin(),
            exchange.initiator_address.end(), addresses.begin());
  std::copy(exchange.responder_address.begin(),
            exchange.responder_address.end(), addresses.begin() + 36);
  const auto ciphertext =
      Cbc(keys.cipher, exchange.address_iv, addresses, true, true);
  std::copy(ciphertext.begin(), ciphertext.end(), records.begin() + 56);
  Write(records.subspan(128, 2), 0x8058);
  Write(records.subspan(130, 2), 100, true);
  std::copy(exchange.public_value.begin(), exchange.public_value.end(),
            records.begin() + 132);
  Write(records.subspan(228, 2), 0x8158);
  Write(records.subspan(230, 2), 24, true);
  const auto tag = HmacSha1(keys.authentication, {records.first(228)});
  std::copy(tag.begin(), tag.end(), records.begin() + 232);
  return wire;
}

std::optional<Exchange> DecodeExchange(const RegistrationKeys& keys,
                                       const KeyId& id,
                                       std::span<const uint8_t> wire) {
  if (wire.size() != 256 || Read(wire.first(4))) {
    return std::nullopt;
  }
  const auto r = wire.subspan(4);
  const uint16_t type = uint16_t(Read(r.first(2)));
  if ((type != 0x0058 && type != 0x0158) ||
      Read(r.subspan(2, 2), true) != 128 || Read(r.subspan(128, 2)) != 0x8058 ||
      Read(r.subspan(130, 2), true) != 100 ||
      Read(r.subspan(228, 2)) != 0x8158 ||
      Read(r.subspan(230, 2), true) != 24 || !Equal(id, r.subspan(8, 8))) {
    return std::nullopt;
  }
  const auto tag = HmacSha1(keys.authentication, {r.first(228)});
  if (!Equal(tag, r.subspan(232, 20))) {
    return std::nullopt;
  }
  Exchange result;
  result.response = type == 0x0158;
  result.id = id;
  result.reserved = uint16_t(Read(r.subspan(4, 2)));
  result.hops = uint16_t(Read(r.subspan(6, 2), true));
  result.initiator_spi = uint32_t(Read(r.subspan(16, 4)));
  result.responder_spi = uint32_t(Read(r.subspan(20, 4)));
  std::copy_n(r.begin() + 24, 8, result.initiator_challenge.begin());
  std::copy_n(r.begin() + 32, 8, result.responder_challenge.begin());
  result.freshness = Read(r.subspan(40, 8), true);
  std::copy_n(r.begin() + 48, 8, result.address_iv.begin());
  if (!ValidExchange(result)) {
    return std::nullopt;
  }
  const auto plain =
      Cbc(keys.cipher, result.address_iv, r.subspan(56, 72), false, true);
  std::copy_n(plain.begin(), 36, result.initiator_address.begin());
  std::copy_n(plain.begin() + 36, 36, result.responder_address.begin());
  std::copy_n(r.begin() + 132, 96, result.public_value.begin());
  return result;
}

bool ResponseMatches(const Exchange& request, const Exchange& response) {
  return !request.response && response.response && ValidExchange(request) &&
         ValidExchange(response) && request.id == response.id &&
         request.initiator_spi == response.initiator_spi &&
         request.initiator_challenge == response.initiator_challenge;
}

bool ReplayWindow::Permitted(uint32_t sequence) const {
  if (!sequence && !allow_zero_) {
    return false;
  }
  if (sequence > highest_) {
    return sequence - highest_ <= 4096;
  }
  const uint32_t age = highest_ - sequence;
  return age < 32 && !(seen_ & (uint32_t(1) << age));
}

std::vector<uint32_t> ReplayWindow::Candidates(uint16_t low) const {
  std::vector<uint32_t> result;
  const int64_t base = int64_t(highest_ & 0xFFFF0000) + low;
  for (int64_t candidate : {base, base - 65536, base + 65536}) {
    if (candidate >= 0 && candidate <= std::numeric_limits<uint32_t>::max() &&
        Permitted(uint32_t(candidate))) {
      result.push_back(uint32_t(candidate));
    }
  }
  return result;
}

bool ReplayWindow::Commit(uint32_t sequence) {
  if (!Permitted(sequence)) {
    return false;
  }
  if (sequence > highest_) {
    const uint32_t advance = sequence - highest_;
    seen_ = advance >= 32 ? 1 : (seen_ << advance) | 1;
    highest_ = sequence;
  } else {
    seen_ |= uint32_t(1) << (highest_ - sequence);
  }
  return true;
}

std::optional<QosHeader> PeekQos(std::span<const uint8_t> wire) {
  if (wire.size() < 4 || Read(wire.first(4))) {
    return std::nullopt;
  }
  const auto record = wire.subspan(4);
  if (record.size() < kQosReplyHeaderSize) {
    return std::nullopt;
  }
  const uint16_t type = uint16_t(Read(record.first(2)));
  QosHeader header;
  if (type == kQosProbeType) {
    if (record.size() < kQosProbeSize) {
      return std::nullopt;
    }
    std::copy_n(record.begin() + 18, 8, header.id.begin());
  } else if (type == kQosReplyType) {
    header.reply = true;
  } else {
    return std::nullopt;
  }
  std::copy_n(record.begin() + 6, 8, header.nonce.begin());
  header.target = uint16_t(Read(record.subspan(14, 2), true));
  return header;
}

std::vector<uint8_t> EncodeQosProbe(const RegistrationKeys& keys,
                                    const QosProbe& probe) {
  std::vector<uint8_t> wire(4 + kQosProbeSize, 0);
  const auto record = std::span(wire).subspan(4);
  Write(record.first(2), kQosProbeType);
  Write(record.subspan(2, 2), kQosProbeSize, true);
  std::copy(probe.nonce.begin(), probe.nonce.end(), record.begin() + 6);
  Write(record.subspan(14, 2), probe.target, true);
  record[16] = probe.sequence;
  record[17] = probe.flags;
  std::copy(probe.id.begin(), probe.id.end(), record.begin() + 18);
  std::array<uint8_t, 40> address{};
  std::copy(probe.address.begin(), probe.address.end(), address.begin());
  const auto ciphertext = Cbc(keys.cipher, probe.nonce, address, true, true);
  std::copy(ciphertext.begin(), ciphertext.end(), record.begin() + 26);
  const auto tag = HmacSha1(keys.authentication, {record.first(66)});
  std::copy_n(tag.begin(), kQosTagSize, record.begin() + 66);
  return wire;
}

std::optional<QosProbe> DecodeQosProbe(const RegistrationKeys& keys,
                                       std::span<const uint8_t> wire) {
  const auto header = PeekQos(wire);
  if (!header || header->reply) {
    return std::nullopt;
  }
  const auto record = wire.subspan(4);
  // The tag also covers whatever a longer record carries after it.
  const size_t size = Read(record.subspan(2, 2), true);
  if (size < kQosProbeSize || size > record.size() ||
      (record[17] & kQosRejectedFlags)) {
    return std::nullopt;
  }
  const auto tag = HmacSha1(
      keys.authentication,
      {record.first(66), record.subspan(kQosProbeSize, size - kQosProbeSize)});
  if (!Equal(std::span(tag).first(kQosTagSize),
             record.subspan(66, kQosTagSize))) {
    return std::nullopt;
  }
  const auto address =
      Cbc(keys.cipher, header->nonce, record.subspan(26, 40), false, true);
  if (std::any_of(address.begin() + 36, address.end(),
                  [](uint8_t byte) { return byte != 0; })) {
    return std::nullopt;
  }
  QosProbe probe;
  probe.nonce = header->nonce;
  probe.target = header->target;
  probe.sequence = record[16];
  probe.flags = record[17];
  probe.id = header->id;
  std::copy_n(address.begin(), 36, probe.address.begin());
  return probe;
}

std::optional<std::vector<uint8_t>> EncodeQosReply(const RegistrationKeys& keys,
                                                   const QosReply& reply) {
  if (!(reply.flags & kQosReplyData) && !reply.data.empty()) {
    return std::nullopt;
  }
  std::vector<uint8_t> plain(reply.data.begin(), reply.data.end());
  plain.resize((plain.size() + 7) & ~size_t(7), 0);
  const size_t size = kQosReplyHeaderSize + plain.size();
  if (size > 0xFFFF) {
    return std::nullopt;
  }
  std::vector<uint8_t> wire(4 + size, 0);
  const auto record = std::span(wire).subspan(4);
  Write(record.first(2), kQosReplyType);
  Write(record.subspan(2, 2), size, true);
  std::copy(reply.nonce.begin(), reply.nonce.end(), record.begin() + 6);
  Write(record.subspan(14, 2), reply.target, true);
  record[16] = reply.sequence;
  record[17] = reply.flags;
  Write(record.subspan(18, 4), reply.delay, true);
  Write(record.subspan(22, 4), reply.gap, true);
  record[26] = uint8_t(plain.size() - reply.data.size());
  if (!plain.empty()) {
    const auto ciphertext = Cbc(keys.cipher, reply.nonce, plain, true, true);
    std::copy(ciphertext.begin(), ciphertext.end(),
              record.begin() + kQosReplyHeaderSize);
  }
  const auto tag =
      HmacSha1(keys.authentication,
               {record.first(27), record.subspan(kQosReplyHeaderSize)});
  std::copy_n(tag.begin(), kQosTagSize, record.begin() + 27);
  return wire;
}

std::optional<QosReply> DecodeQosReply(const RegistrationKeys& keys,
                                       std::span<const uint8_t> wire) {
  const auto header = PeekQos(wire);
  if (!header || !header->reply) {
    return std::nullopt;
  }
  const auto record = wire.subspan(4);
  const size_t size = Read(record.subspan(2, 2), true);
  if (size < kQosReplyHeaderSize || size > record.size() ||
      (record[17] & kQosRejectedFlags)) {
    return std::nullopt;
  }
  const auto ciphertext =
      record.subspan(kQosReplyHeaderSize, size - kQosReplyHeaderSize);
  const size_t padding = record[26];
  if ((record[17] & kQosReplyData)
          ? padding > 7 || padding > ciphertext.size() || ciphertext.size() % 8
          : padding || !ciphertext.empty()) {
    return std::nullopt;
  }
  const auto tag =
      HmacSha1(keys.authentication, {record.first(27), ciphertext});
  if (!Equal(std::span(tag).first(kQosTagSize),
             record.subspan(27, kQosTagSize))) {
    return std::nullopt;
  }
  QosReply reply;
  reply.nonce = header->nonce;
  reply.target = header->target;
  reply.sequence = record[16];
  reply.flags = record[17];
  reply.delay = uint32_t(Read(record.subspan(18, 4), true));
  reply.gap = uint32_t(Read(record.subspan(22, 4), true));
  if (!ciphertext.empty()) {
    const auto plain = Cbc(keys.cipher, reply.nonce, ciphertext, false, true);
    reply.data.assign(plain.begin(), plain.end() - padding);
  }
  return reply;
}

}  // namespace xe::net::secure
