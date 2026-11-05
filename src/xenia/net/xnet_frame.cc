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

namespace xe::net::secure {
namespace {

constexpr Mac kBroadcast{255, 255, 255, 255, 255, 255};

uint16_t Word(std::span<const uint8_t> bytes) {
  return uint16_t((uint16_t(bytes[0]) << 8) | bytes[1]);
}

void SetWord(std::span<uint8_t> bytes, uint16_t value) {
  bytes[0] = uint8_t(value >> 8);
  bytes[1] = uint8_t(value);
}

uint16_t Checksum(std::span<const uint8_t> bytes, uint32_t sum = 0) {
  for (size_t i = 0; i < bytes.size(); i += 2) {
    sum +=
        (uint32_t(bytes[i]) << 8) | (i + 1 < bytes.size() ? bytes[i + 1] : 0);
  }
  while (sum >> 16) {
    sum = (sum & 0xFFFF) + (sum >> 16);
  }
  return uint16_t(~sum);
}

uint32_t PseudoHeader(std::span<const uint8_t> ip, uint16_t udp_length) {
  return uint32_t(Word(ip.subspan(12, 2))) + Word(ip.subspan(14, 2)) +
         Word(ip.subspan(16, 2)) + Word(ip.subspan(18, 2)) + 17 + udp_length;
}

bool Unicast(const Mac& mac) {
  return !(mac[0] & 1) &&
         std::ranges::any_of(mac, [](uint8_t byte) { return byte != 0; });
}

uint32_t Ipv4(std::span<const uint8_t> bytes) {
  return uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 |
         uint32_t(bytes[2]) << 8 | bytes[3];
}

void SetIpv4(std::span<uint8_t> bytes, uint32_t address) {
  for (size_t i = 0; i < 4; ++i) {
    bytes[i] = uint8_t(address >> (24 - 8 * i));
  }
}

// The IP an IpMachine() holds, 0 for any other machine.
uint32_t MachineIp(const Mac& machine) {
  return machine[0] == 0x02 && machine[1] == 0
             ? Ipv4(std::span(machine).subspan(2))
             : 0;
}

// Whether a cross-platform frame may come from |source|.
bool SubnetPeer(uint32_t source, const CrossPlatform& cross_platform) {
  const uint32_t mask = cross_platform.mask;
  return cross_platform.address && mask && source != cross_platform.address &&
         (source & mask) == (cross_platform.address & mask) &&
         (source | mask) != 0xFFFFFFFF && (source >> 24) != 127;
}

}  // namespace

Mac IpMachine(uint32_t address) {
  // Locally administered so no network card has it.
  return {0x02,
          0x00,
          uint8_t(address >> 24),
          uint8_t(address >> 16),
          uint8_t(address >> 8),
          uint8_t(address)};
}

bool SubnetPeer(const Address& address, const CrossPlatform& cross_platform) {
  return SubnetPeer(Ipv4(address), cross_platform);
}

Mac Machine(const Address& address, bool cross_platform) {
  if (cross_platform) {
    return IpMachine(Ipv4(address));
  }
  Mac mac;
  std::copy_n(address.begin() + 10, mac.size(), mac.begin());
  return mac;
}

bool SameMachine(const Address& a, const Address& b, bool cross_platform) {
  return Machine(a, false) == Machine(b, false) &&
         (!cross_platform || Ipv4(a) == Ipv4(b));
}

bool BroadcastPayload(std::span<const uint8_t> payload) {
  return payload.size() >= 4 && payload[0] && !payload[1] && !payload[2] &&
         !payload[3];
}

std::optional<std::vector<uint8_t>> EncodeFrame(
    const Frame& frame, const std::optional<CrossPlatform>& cross_platform) {
  if (!Unicast(frame.source) ||
      (frame.broadcast ? frame.destination != kBroadcast
                       : !Unicast(frame.destination)) ||
      frame.payload.size() > kMaxDatagramSize) {
    return std::nullopt;
  }
  if (cross_platform && (!MachineIp(frame.source) ||
                         (!frame.broadcast && !MachineIp(frame.destination)))) {
    return std::nullopt;
  }
  std::vector<uint8_t> result(42 + frame.payload.size(), 0);
  std::copy(frame.destination.begin(), frame.destination.end(), result.begin());
  std::copy(frame.source.begin(), frame.source.end(), result.begin() + 6);
  SetWord(std::span(result).subspan(12, 2), 0x0800);
  auto ip = std::span(result).subspan(14, 20);
  ip[0] = 0x45;
  SetWord(ip.subspan(2, 2), uint16_t(28 + frame.payload.size()));
  ip[8] = 128;
  ip[9] = 17;
  if (cross_platform) {
    SetIpv4(ip.subspan(12, 4), MachineIp(frame.source));
  } else {
    // Consoles address each other as 0.0.0.1 and tell machines apart by MAC.
    ip[15] = 1;
  }
  if (frame.broadcast) {
    std::fill(ip.begin() + 16, ip.end(), 255);
  } else if (cross_platform) {
    SetIpv4(ip.subspan(16, 4), MachineIp(frame.destination));
  } else {
    ip[19] = 1;
  }
  SetWord(ip.subspan(10, 2), Checksum(ip));
  auto udp = std::span(result).subspan(34);
  const uint16_t port = cross_platform ? cross_platform->port : kSystemLinkPort;
  SetWord(udp.first(2), port);
  SetWord(udp.subspan(2, 2), port);
  SetWord(udp.subspan(4, 2), uint16_t(udp.size()));
  std::copy(frame.payload.begin(), frame.payload.end(), udp.begin() + 8);
  const auto checksum = Checksum(udp, PseudoHeader(ip, uint16_t(udp.size())));
  SetWord(udp.subspan(6, 2), checksum ? checksum : 0xFFFF);
  return result;
}

std::optional<Frame> DecodeFrame(
    std::span<const uint8_t> wire,
    const std::optional<CrossPlatform>& cross_platform) {
  if (wire.size() < 42 || Word(wire.subspan(12, 2)) != 0x0800) {
    return std::nullopt;
  }
  const auto ip = wire.subspan(14, 20);
  const auto length = Word(ip.subspan(2, 2));
  if (ip[0] != 0x45 || ip[9] != 17 || !ip[8] || length < 28 ||
      length > wire.size() - 14 || (Word(ip.subspan(6, 2)) & 0xBFFF) ||
      Checksum(ip)) {
    return std::nullopt;
  }
  const uint32_t source = Ipv4(ip.subspan(12, 4));
  const uint32_t destination = Ipv4(ip.subspan(16, 4));
  const bool broadcast = destination == 0xFFFFFFFF;
  if (cross_platform
          ? !SubnetPeer(source, *cross_platform) ||
                (!broadcast && destination != cross_platform->address)
          : source != 1 || (!broadcast && destination != 1)) {
    return std::nullopt;
  }
  const auto udp = wire.subspan(34, length - 20);
  const uint16_t port = cross_platform ? cross_platform->port : kSystemLinkPort;
  if (Word(udp.first(2)) != port || Word(udp.subspan(2, 2)) != port ||
      Word(udp.subspan(4, 2)) != udp.size() ||
      (Word(udp.subspan(6, 2)) &&
       Checksum(udp, PseudoHeader(ip, uint16_t(udp.size()))))) {
    return std::nullopt;
  }
  Frame frame;
  if (cross_platform) {
    frame.source = IpMachine(source);
    frame.destination = broadcast ? kBroadcast : IpMachine(destination);
  } else {
    std::copy_n(wire.begin(), 6, frame.destination.begin());
    std::copy_n(wire.begin() + 6, 6, frame.source.begin());
    if (!Unicast(frame.source) || (broadcast ? frame.destination != kBroadcast
                                             : !Unicast(frame.destination))) {
      return std::nullopt;
    }
  }
  frame.broadcast = broadcast;
  frame.payload.assign(udp.begin() + 8, udp.end());
  return frame;
}

}  // namespace xe::net::secure
