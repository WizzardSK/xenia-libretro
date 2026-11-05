/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/net/host_interfaces.h"

#include <algorithm>
#include <utility>

#include <asio.hpp>

namespace xe::net {
namespace {

// A connected UDP socket sends nothing but the system still picks the address
// it would send from.
uint32_t DefaultRouteAddress() {
  asio::io_context io;
  asio::ip::udp::socket socket(io);
  asio::error_code ec;
  socket.open(asio::ip::udp::v4(), ec);
  if (!ec) {
    // TEST-NET-1, which only a default route leads to.
    socket.connect({asio::ip::address_v4(0xC0000201), 9}, ec);
  }
  if (ec) {
    return 0;
  }
  const auto local = socket.local_endpoint(ec);
  return ec ? 0 : local.address().to_v4().to_uint();
}

}  // namespace

std::optional<HostInterface> FindHostInterface(std::string_view name) {
  const uint32_t address = name.empty() ? DefaultRouteAddress() : 0;
  if (name.empty() && !address) {
    return std::nullopt;
  }
  for (const auto& host : HostInterfaces()) {
    if (name.empty() ? host.address == address : host.name == name) {
      return host;
    }
  }
  return std::nullopt;
}

std::vector<std::string> HostInterfaceNames() {
  std::vector<std::string> result;
  for (auto& host : HostInterfaces()) {
    if (std::find(result.begin(), result.end(), host.name) == result.end()) {
      result.push_back(std::move(host.name));
    }
  }
  return result;
}

std::vector<uint32_t> SubnetBroadcastAddresses() {
  std::vector<uint32_t> result;
  for (const auto& host : HostInterfaces()) {
    const uint32_t host_bits = ~host.mask;
    // A /31 or /32 has no broadcast address and a /0 would give the limited
    // one.
    if (host_bits <= 1 || host_bits == 0xFFFFFFFF) {
      continue;
    }
    const uint32_t broadcast = host.address | host_bits;
    if (std::find(result.begin(), result.end(), broadcast) == result.end()) {
      result.push_back(broadcast);
    }
  }
  return result;
}

}  // namespace xe::net
