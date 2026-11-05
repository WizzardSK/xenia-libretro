/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_NET_XNET_SECURE_H_
#define XENIA_NET_XNET_SECURE_H_

#include <array>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace xe::net {

using KeyId = std::array<uint8_t, 8>;
using Key = std::array<uint8_t, 16>;

inline constexpr size_t kMaxDatagramSize = 65507;

// Fills |bytes| from a nondeterministic random source.
void Random(std::span<uint8_t> bytes);

}  // namespace xe::net

namespace xe::net::secure {

using Block = std::array<uint8_t, 8>;
using Address = std::array<uint8_t, 36>;
using DhValue = std::array<uint8_t, 96>;
using Metadata = std::array<uint8_t, 12>;
using Mac = std::array<uint8_t, 6>;
using Sha1Digest = std::array<uint8_t, 20>;
using PlatformHmac = std::function<bool(std::span<const uint8_t>, Sha1Digest&)>;

Sha1Digest HmacSha1(std::span<const uint8_t> key,
                    std::initializer_list<std::span<const uint8_t>> parts);

// RFC 2409 group 1 key agreement with a 256-bit private exponent. Values on
// the wire are little endian.
class DhKeyPair {
 public:
  static std::optional<DhKeyPair> Generate();
  static std::optional<DhKeyPair> FromPrivate(
      std::span<const uint8_t> big_endian_exponent);

  const DhValue& public_value() const { return public_; }
  std::optional<DhValue> Shared(const DhValue& peer) const;

 private:
  struct Context;
  struct ContextDeleter {
    void operator()(Context* context) const;
  };

  DhKeyPair() = default;

  std::unique_ptr<Context, ContextDeleter> context_;
  DhValue public_{};
};

struct ProtectionKeys {
  Key authentication{};
  std::array<uint8_t, 24> cipher{};
  Block iv_seed{};
};

struct RegistrationKeys {
  Key authentication{};
  std::array<uint8_t, 24> cipher{};
};

struct TrafficKeys {
  ProtectionKeys a, b;
};

std::optional<ProtectionKeys> DeriveBroadcast(
    const Key& executable_key, const PlatformHmac& platform_hmac);
RegistrationKeys DeriveRegistration(const Key& broadcast_auth, const KeyId& id,
                                    const Key& key);
TrafficKeys DeriveTraffic(const Key& registration_auth, const DhValue& shared,
                          const Block& initiator, const Block& responder);
Block PacketIv(const Block& seed, uint32_t sequence);
Metadata MakeMetadata(uint32_t executable_id, uint32_t version, uint16_t major,
                      uint16_t minor, uint16_t build, uint8_t revision);
// The metadata a discovery packet sends in the clear, none for a packet that
// isn't laid out as one.
std::optional<Metadata> PeekMetadata(std::span<const uint8_t> wire);
// XNetGetBroadcastVersionStatus bits.
inline constexpr uint32_t kBroadcastVersionOlder = 0x01;
inline constexpr uint32_t kBroadcastVersionNewer = 0x02;
// Whether a broadcast from |theirs| came from an older or newer version or
// kernel than |own|, 0 for another title.
uint32_t BroadcastVersionDifference(const Metadata& own,
                                    const Metadata& theirs);
bool FreshnessNewer(uint64_t incoming, uint64_t previous, bool response);

inline constexpr uint16_t kSystemLinkPort = 3074;

// Titles with cross-platform System Link talk over the real IPs of one subnet
// on their System Link port. Host byte order.
struct CrossPlatform {
  uint32_t address = 0, mask = 0;
  uint16_t port = kSystemLinkPort;
};

// System Link key ids carry this type in their top three bits.
constexpr uint8_t SystemLinkKeyType(bool cross_platform) {
  return cross_platform ? 0x40 : 0x00;
}
constexpr bool SystemLinkKeyId(const KeyId& id, bool cross_platform) {
  return (id[0] & 0xE0) == SystemLinkKeyType(cross_platform);
}

// Frames name a machine by its MAC, cross-platform ones by IpMachine() of its
// IP.
Mac IpMachine(uint32_t address);
// The machine an XNADDR names.
Mac Machine(const Address& address, bool cross_platform);
// Whether |a| and |b| name one machine by its MAC and for cross-platform
// System Link also by its IP. xam ignores the other fields, which titles may
// garble.
bool SameMachine(const Address& a, const Address& b, bool cross_platform);
// Whether the IP of |address| belongs to a peer of the subnet, which isn't
// this machine, the subnet's broadcast address or a loopback address.
bool SubnetPeer(const Address& address, const CrossPlatform& cross_platform);

struct Frame {
  Mac source{}, destination{};
  bool broadcast = false;
  std::vector<uint8_t> payload;
};

// Whether a frame's payload uses the broadcast keys, which only broadcasts do.
bool BroadcastPayload(std::span<const uint8_t> payload);

std::optional<std::vector<uint8_t>> EncodeFrame(
    const Frame& frame,
    const std::optional<CrossPlatform>& cross_platform = std::nullopt);
// A cross-platform frame must come from another machine of the subnet on the
// System Link port and go to this machine or 255.255.255.255.
std::optional<Frame> DecodeFrame(
    std::span<const uint8_t> wire,
    const std::optional<CrossPlatform>& cross_platform = std::nullopt);

enum class Kind { Control, Tcp, Udp, Vdp, Discovery };

// The part of a TCP header that follows the ports. There is no checksum or
// urgent pointer.
struct TcpHeader {
  uint32_t sequence = 0, acknowledgment = 0;
  // Header length in 32-bit words, options included, in the high nibble.
  uint8_t offset = 0x50;
  uint8_t flags = 0;
  uint16_t window = 0;
  bool operator==(const TcpHeader&) const = default;
};

struct Packet {
  Kind kind = Kind::Udp;
  uint16_t source_port = 1000, destination_port = 1000;
  uint16_t control_type = 0;
  // TCP options lead |encrypted|, the segment's data follows them.
  TcpHeader tcp;
  std::vector<uint8_t> encrypted, clear;
  Metadata metadata{};
};

// Nothing is decoded unless the tag authenticates.
bool Encode(const ProtectionKeys& keys, uint32_t spi, uint32_t sequence,
            const Packet& packet, std::vector<uint8_t>& wire);
std::optional<Packet> Decode(const ProtectionKeys& keys, uint32_t spi,
                             uint32_t sequence, std::span<const uint8_t> wire,
                             bool discovery = false);

// Guest VDP datagrams start with the big endian size of the encrypted part,
// the rest is sent in the clear.
std::optional<Packet> FromGuestVdp(uint16_t source, uint16_t destination,
                                   std::span<const uint8_t> guest);
std::vector<uint8_t> ToGuestVdp(const Packet& packet);

struct Exchange {
  bool response = false;
  uint16_t reserved = 0, hops = 127;
  KeyId id{};
  uint32_t initiator_spi = 0, responder_spi = 0;
  Block initiator_challenge{}, responder_challenge{};
  uint64_t freshness = 0;
  Block address_iv{};
  Address initiator_address{}, responder_address{};
  DhValue public_value{};
};

std::optional<std::array<uint8_t, 256>> EncodeExchange(
    const RegistrationKeys& keys, const Exchange& exchange);
// Checks the tag and record syntax only. The caller binds the addresses,
// challenges, identifier and SPIs to its own pending state.
std::optional<Exchange> DecodeExchange(const RegistrationKeys& keys,
                                       const KeyId& id,
                                       std::span<const uint8_t> wire);
// The responder names itself by its own XNADDR and the initiator by the one it
// keeps for it, which the caller matches by machine.
bool ResponseMatches(const Exchange& request, const Exchange& response);

// QoS records follow a zero SPI like exchanges and use the registration keys
// of the key id they name. A prober asks a host for the data it listens with,
// or times the path with probes sent in pairs.
inline constexpr uint8_t kQosProbeSecond = 0x01;
inline constexpr uint8_t kQosProbeDataRequest = 0x02;
inline constexpr uint8_t kQosReplySecond = 0x01;
inline constexpr uint8_t kQosReplyData = 0x04;
// The prober then has no data to ask for.
inline constexpr uint8_t kQosReplyNoData = 0x08;
inline constexpr uint8_t kQosReplyDisabled = 0x10;

struct QosProbe {
  // Per lookup and echoed by replies.
  Block nonce{};
  uint16_t target = 0;
  uint8_t sequence = 0;
  uint8_t flags = 0;
  KeyId id{};
  // The host's XNADDR, which only that host accepts.
  Address address{};
};

struct QosReply {
  Block nonce{};
  uint16_t target = 0;
  uint8_t sequence = 0;
  uint8_t flags = 0;
  // Microseconds the host held the probe, and between the probes of a pair.
  uint32_t delay = 0, gap = 0;
  std::vector<uint8_t> data;
};

// The clear part of a QoS record, which picks its keys.
struct QosHeader {
  bool reply = false;
  Block nonce{};
  uint16_t target = 0;
  // Probes only.
  KeyId id{};
};

std::optional<QosHeader> PeekQos(std::span<const uint8_t> wire);
std::vector<uint8_t> EncodeQosProbe(const RegistrationKeys& keys,
                                    const QosProbe& probe);
// Checks the tag and that the address decrypts cleanly. The caller checks
// that it names this console.
std::optional<QosProbe> DecodeQosProbe(const RegistrationKeys& keys,
                                       std::span<const uint8_t> wire);
std::optional<std::vector<uint8_t>> EncodeQosReply(const RegistrationKeys& keys,
                                                   const QosReply& reply);
std::optional<QosReply> DecodeQosReply(const RegistrationKeys& keys,
                                       std::span<const uint8_t> wire);

// One window per receive direction and generation. Candidates() never changes
// state, Commit() follows a successful decode.
class ReplayWindow {
 public:
  explicit ReplayWindow(bool allow_zero = false) : allow_zero_(allow_zero) {}

  std::vector<uint32_t> Candidates(uint16_t low) const;
  bool Commit(uint32_t authenticated_sequence);
  uint32_t highest() const { return highest_; }

 private:
  bool Permitted(uint32_t sequence) const;

  uint32_t highest_ = 0, seen_ = 0;
  bool allow_zero_ = false;
};

}  // namespace xe::net::secure

#endif  // XENIA_NET_XNET_SECURE_H_
