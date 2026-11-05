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

#include "xenia/base/platform_win.h"
#include "xenia/base/string.h"

// iphlpapi.h needs the winsock types first.
#include <winsock2.h>  // NOLINT(build/include_order)

#include <iphlpapi.h>  // NOLINT(build/include_order)

namespace xe::net {

std::vector<HostInterface> HostInterfaces() {
  std::vector<HostInterface> result;
  constexpr ULONG kFlags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                           GAA_FLAG_SKIP_DNS_SERVER;
  ULONG size = 16 * 1024;
  std::vector<uint8_t> buffer;
  ULONG status;
  do {
    buffer.resize(size);
    status = GetAdaptersAddresses(
        AF_INET, kFlags, nullptr,
        reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
  } while (status == ERROR_BUFFER_OVERFLOW);
  if (status != NO_ERROR) {
    return result;
  }
  for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
       adapter; adapter = adapter->Next) {
    // Point-to-point links and layer 3 VPN adapters can't broadcast.
    if (adapter->OperStatus != IfOperStatusUp ||
        adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK ||
        adapter->IfType == IF_TYPE_PPP || adapter->IfType == IF_TYPE_TUNNEL ||
        adapter->IfType == IF_TYPE_PROP_VIRTUAL) {
      continue;
    }
    for (auto* unicast = adapter->FirstUnicastAddress; unicast;
         unicast = unicast->Next) {
      const uint8_t prefix = unicast->OnLinkPrefixLength;
      if (unicast->Address.lpSockaddr->sa_family != AF_INET || prefix > 32) {
        continue;
      }
      const auto* address =
          reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
      HostInterface host;
      host.name =
          xe::to_utf8(reinterpret_cast<const char16_t*>(adapter->FriendlyName));
      host.address = ntohl(address->sin_addr.s_addr);
      host.mask = prefix ? 0xFFFFFFFFu << (32 - prefix) : 0;
      host.index = adapter->IfIndex;
      if (adapter->PhysicalAddressLength == host.mac.size()) {
        std::copy_n(adapter->PhysicalAddress, host.mac.size(),
                    host.mac.begin());
      }
      result.push_back(host);
    }
  }
  return result;
}

}  // namespace xe::net
