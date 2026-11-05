/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_NET_HOST_INTERFACES_H_
#define XENIA_NET_HOST_INTERFACES_H_

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xe::net {

// An IPv4 address of a host interface. Host byte order.
struct HostInterface {
  // What the system calls the interface.
  std::string name;
  uint32_t address = 0, mask = 0;
  std::array<uint8_t, 6> mac{};
  // The system's index of the interface.
  uint32_t index = 0;
};

// Each address of the interfaces that are up and can broadcast, loopback
// aside.
std::vector<HostInterface> HostInterfaces();
// Their names, each once.
std::vector<std::string> HostInterfaceNames();

// The first address of the interface named |name|, or of the one with the
// default route when |name| is empty.
std::optional<HostInterface> FindHostInterface(std::string_view name);

// The broadcast address of each subnet the host is on.
std::vector<uint32_t> SubnetBroadcastAddresses();

}  // namespace xe::net

#endif  // XENIA_NET_HOST_INTERFACES_H_
