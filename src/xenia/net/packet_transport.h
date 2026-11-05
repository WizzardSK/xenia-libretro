/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_NET_PACKET_TRANSPORT_H_
#define XENIA_NET_PACKET_TRANSPORT_H_

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "xenia/net/host_interfaces.h"
#include "xenia/net/xnet_secure.h"

namespace xe::net::secure {

// Carries this console's Ethernet frames to and from the System Link LAN.
class PacketTransport {
 public:
  virtual ~PacketTransport() = default;
  virtual Mac identity() const = 0;
  virtual uint32_t mtu() const = 0;
  virtual bool healthy() const { return true; }
  // Returns the bytes sent or -1.
  virtual int Send(std::span<const uint8_t> frame) = 0;
  // False on timeout, error or Wake().
  virtual bool Receive(std::vector<uint8_t>& frame, uint32_t timeout_ms) = 0;
  // Ends the Receive waiting on another thread, or the next one if none is.
  // Thread safe.
  virtual void Wake() = 0;
  // Moves a transport on the host network to another UDP port.
  virtual bool SetPort(uint16_t port) { return false; }
};

// Host byte order.
struct UdpEndpoint {
  uint32_t address = 0;
  uint16_t port = 0;
};

inline constexpr UdpEndpoint kKaiEngine{0x7F000001, 34523};

// Exchanges frames with an XLink Kai engine over its DDS protocol. Kai drops
// frames whose source MAC is not from a console vendor.
std::unique_ptr<PacketTransport> OpenKaiTransport(const Mac& mac,
                                                  UdpEndpoint engine,
                                                  std::string& error);

// Sends every frame as a UDP datagram to |destinations| and receives on
// |port|, which several instances on one machine can share. Reaches other
// Xenia instances only.
std::unique_ptr<PacketTransport> OpenUdpTunnelTransport(
    const Mac& mac, uint16_t port, std::vector<UdpEndpoint> destinations,
    std::string& error);

// Sends the datagram of each cross-platform frame on the host network from
// |host|'s address, on UDP |port| both ways, and receives the datagrams that
// arrive there as frames.
std::unique_ptr<PacketTransport> OpenHostTransport(const HostInterface& host,
                                                   uint16_t port,
                                                   std::string& error);

}  // namespace xe::net::secure

#endif  // XENIA_NET_PACKET_TRANSPORT_H_
