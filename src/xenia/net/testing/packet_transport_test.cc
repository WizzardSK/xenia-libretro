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
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <asio.hpp>

#include "xenia/base/platform.h"
#include "xenia/net/host_interfaces.h"
#include "xenia/net/packet_transport.h"
#include "xenia/net/system_link.h"

#include "third_party/catch/include/catch.hpp"

#if !XE_PLATFORM_WIN32
#include <net/if.h>
#endif

namespace xe::net::secure::test {
namespace {

using udp = asio::ip::udp;

constexpr uint32_t kLoopback = 0x7F000001;
constexpr Mac kMacA{2, 17, 34, 51, 68, 1}, kMacB{2, 17, 34, 51, 68, 2};

std::vector<uint8_t> MakeFrameTo(const Mac& source, const Mac& destination,
                                 uint8_t fill) {
  std::vector<uint8_t> frame(60, fill);
  std::copy(destination.begin(), destination.end(), frame.begin());
  std::copy(source.begin(), source.end(), frame.begin() + 6);
  return frame;
}

std::vector<uint8_t> MakeFrame(const Mac& source, uint8_t fill) {
  return MakeFrameTo(source, {255, 255, 255, 255, 255, 255}, fill);
}

std::string Text(std::span<const uint8_t> bytes) {
  return std::string(bytes.begin(), bytes.end());
}

// A loopback socket standing in for the far end.
struct Peer {
  asio::io_context io;
  udp::socket socket{io, udp::endpoint(asio::ip::address_v4::loopback(), 0)};
  udp::endpoint last_sender;

  uint16_t port() const { return socket.local_endpoint().port(); }

  void SendTo(std::string_view data, const udp::endpoint& destination) {
    socket.send_to(asio::buffer(data.data(), data.size()), destination);
  }

  std::optional<std::string> Receive(uint32_t timeout_ms) {
    std::vector<char> buffer(65536);
    std::optional<std::string> result;
    socket.async_receive_from(asio::buffer(buffer), last_sender,
                              [&](const asio::error_code& ec, size_t size) {
                                if (!ec) {
                                  result.emplace(buffer.data(), size);
                                }
                              });
    io.restart();
    io.run_for(std::chrono::milliseconds(timeout_ms));
    if (!result) {
      socket.cancel();
      io.run();
    }
    return result;
  }
};

uint16_t FreePort() {
  Peer probe;
  return probe.port();
}

HostInterface Loopback() {
  HostInterface host;
  host.address = kLoopback;
  host.mask = 0xFF000000;
  host.mac = kMacA;
#if XE_PLATFORM_WIN32
  host.index = 1;
#elif XE_PLATFORM_MAC
  host.index = if_nametoindex("lo0");
#else
  host.index = if_nametoindex("lo");
#endif
  return host;
}

}  // namespace

TEST_CASE("UDP tunnel carries frames to other instances", "[net][transport]") {
  const uint16_t port_a = FreePort();
  uint16_t port_b = FreePort();
  while (port_b == port_a) {
    port_b = FreePort();
  }
  std::string error;
  // A also sends to itself, as a broadcast would.
  auto a = OpenUdpTunnelTransport(
      kMacA, port_a, {{kLoopback, port_b}, {kLoopback, port_a}}, error);
  auto b = OpenUdpTunnelTransport(kMacB, port_b, {{kLoopback, port_a}}, error);
  REQUIRE(a);
  REQUIRE(b);
  CHECK(a->identity() == kMacA);

  const auto frame = MakeFrame(kMacA, 7);
  REQUIRE(a->Send(frame) == static_cast<int>(frame.size()));
  std::vector<uint8_t> received;
  REQUIRE(b->Receive(received, 2000));
  CHECK(received == frame);
  CHECK_FALSE(a->Receive(received, 100));

  const auto reply = MakeFrame(kMacB, 9);
  REQUIRE(b->Send(reply) == static_cast<int>(reply.size()));
  REQUIRE(a->Receive(received, 2000));
  CHECK(received == reply);

  // Datagrams without the tunnel tag are someone else's.
  Peer stranger;
  stranger.SendTo(Text(frame), {asio::ip::address_v4::loopback(), port_b});
  CHECK_FALSE(b->Receive(received, 100));

  // Instances on one machine share the port.
  CHECK(OpenUdpTunnelTransport(kMacB, port_a, {}, error));
  CHECK(a->healthy());
}

TEST_CASE("UDP tunnel sends straight to the peers it heard",
          "[net][transport]") {
  // The observer stands in for the rest of the LAN that broadcasts reach.
  Peer observer;
  const uint16_t port_a = FreePort();
  uint16_t port_b = FreePort();
  while (port_b == port_a) {
    port_b = FreePort();
  }
  std::string error;
  auto a = OpenUdpTunnelTransport(
      kMacA, port_a, {{kLoopback, port_b}, {kLoopback, observer.port()}},
      error);
  auto b = OpenUdpTunnelTransport(kMacB, port_b, {{kLoopback, port_a}}, error);
  REQUIRE(a);
  REQUIRE(b);
  std::vector<uint8_t> received;

  // Until A hears from B, frames for B go everywhere.
  const auto early = MakeFrameTo(kMacA, kMacB, 1);
  REQUIRE(a->Send(early) == static_cast<int>(early.size()));
  REQUIRE(b->Receive(received, 2000));
  CHECK(received == early);
  CHECK(observer.Receive(2000));

  const auto hello = MakeFrame(kMacB, 2);
  REQUIRE(b->Send(hello) == static_cast<int>(hello.size()));
  REQUIRE(a->Receive(received, 2000));
  const auto direct = MakeFrameTo(kMacA, kMacB, 3);
  REQUIRE(a->Send(direct) == static_cast<int>(direct.size()));
  REQUIRE(b->Receive(received, 2000));
  CHECK(received == direct);
  CHECK_FALSE(observer.Receive(200));

  // Broadcasts still go everywhere.
  const auto broadcast = MakeFrame(kMacA, 4);
  REQUIRE(a->Send(broadcast) == static_cast<int>(broadcast.size()));
  REQUIRE(b->Receive(received, 2000));
  CHECK(received == broadcast);
  CHECK(observer.Receive(2000));

  // Instances sharing a port on one machine share an address, which only a
  // broadcast reaches all of.
  const Mac mac_c{2, 17, 34, 51, 68, 3}, mac_d{2, 17, 34, 51, 68, 4};
  Peer shared;
  for (const auto& mac : {mac_c, mac_d}) {
    shared.SendTo("XESL" + Text(MakeFrame(mac, 5)),
                  {asio::ip::address_v4::loopback(), port_a});
    REQUIRE(a->Receive(received, 2000));
  }
  const auto to_shared = MakeFrameTo(kMacA, mac_c, 6);
  REQUIRE(a->Send(to_shared) == static_cast<int>(to_shared.size()));
  CHECK(observer.Receive(2000));
  REQUIRE(b->Receive(received, 2000));
  CHECK(received == to_shared);

  // A group address can't send, so it can't take broadcasts off the LAN.
  Peer rogue;
  rogue.SendTo("XESL" + Text(MakeFrame({255, 255, 255, 255, 255, 255}, 7)),
               {asio::ip::address_v4::loopback(), port_a});
  CHECK_FALSE(a->Receive(received, 200));
  const auto after_rogue = MakeFrame(kMacA, 8);
  REQUIRE(a->Send(after_rogue) == static_cast<int>(after_rogue.size()));
  CHECK(observer.Receive(2000));
  REQUIRE(b->Receive(received, 2000));
  CHECK(received == after_rogue);

  // An instance that comes back under a new MAC gets frames straight again
  // once the MACs it had before go quiet.
  std::this_thread::sleep_for(std::chrono::milliseconds(3200));
  const Mac mac_e{2, 17, 34, 51, 68, 5};
  shared.SendTo("XESL" + Text(MakeFrame(mac_e, 9)),
                {asio::ip::address_v4::loopback(), port_a});
  REQUIRE(a->Receive(received, 2000));
  const auto to_restarted = MakeFrameTo(kMacA, mac_e, 10);
  REQUIRE(a->Send(to_restarted) == static_cast<int>(to_restarted.size()));
  CHECK(shared.Receive(2000));
  CHECK_FALSE(observer.Receive(200));
}

TEST_CASE("Transports stop waiting for a frame when woken",
          "[net][transport]") {
  Peer engine;
  std::string error;
  std::unique_ptr<PacketTransport> transports[] = {
      OpenUdpTunnelTransport(kMacA, FreePort(), {{kLoopback, FreePort()}},
                             error),
      OpenKaiTransport(kMacA, {kLoopback, engine.port()}, error),
      OpenHostTransport(Loopback(), FreePort(), error)};
  for (auto& transport : transports) {
    REQUIRE(transport);
    for (bool before : {true, false}) {
      if (before) {
        transport->Wake();
      }
      auto waiting = std::async(std::launch::async, [&] {
        std::vector<uint8_t> frame;
        return transport->Receive(frame, 10000);
      });
      if (!before) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        transport->Wake();
      }
      REQUIRE(waiting.wait_for(std::chrono::seconds(1)) ==
              std::future_status::ready);
      CHECK_FALSE(waiting.get());
    }
    CHECK(transport->healthy());
  }
}

TEST_CASE("Host transport keeps a wake across a port change",
          "[net][transport]") {
  std::string error;
  auto transport = OpenHostTransport(Loopback(), FreePort(), error);
  REQUIRE(transport);
  transport->Wake();
  REQUIRE(transport->SetPort(FreePort()));
  auto waiting = std::async(std::launch::async, [&] {
    std::vector<uint8_t> frame;
    return transport->Receive(frame, 10000);
  });
  REQUIRE(waiting.wait_for(std::chrono::seconds(1)) ==
          std::future_status::ready);
  CHECK_FALSE(waiting.get());
}

TEST_CASE("Host transport carries datagrams as cross-platform frames",
          "[net][transport]") {
  const auto host = Loopback();
  CrossPlatform addressing{host.address, host.mask, FreePort()};
  std::string error;
  auto transport = OpenHostTransport(host, addressing.port, error);
  REQUIRE(transport);
  CHECK(transport->identity() == kMacA);
  auto frame = [&](std::vector<uint8_t> payload) {
    auto encoded = EncodeFrame(
        {IpMachine(kLoopback), IpMachine(kLoopback), false, std::move(payload)},
        addressing);
    REQUIRE(encoded);
    return *encoded;
  };

  // Peers only send from the System Link port.
  Peer stranger;
  stranger.SendTo(std::string_view("\0\0\0\0stray", 9),
                  {asio::ip::address_v4::loopback(), addressing.port});
  const auto exchange = frame({0, 0, 0, 0, 1, 2, 3});
  REQUIRE(transport->Send(exchange) == static_cast<int>(exchange.size()));
  std::vector<uint8_t> received;
  REQUIRE(transport->Receive(received, 1000));
  CHECK(received == exchange);

  // Only broadcasts use the broadcast keys.
  REQUIRE(transport->Send(frame({0x87, 0, 0, 0, 1, 2, 3})) > 0);
  REQUIRE(transport->Receive(received, 1000));
  REQUIRE(received.size() > 34);
  CHECK(std::all_of(received.begin(), received.begin() + 6,
                    [](uint8_t byte) { return byte == 255; }));
  CHECK(std::all_of(received.begin() + 30, received.begin() + 34,
                    [](uint8_t byte) { return byte == 255; }));

  addressing.port = FreePort();
  REQUIRE(transport->SetPort(addressing.port));
  const auto moved = frame({0, 0, 0, 0, 4, 5, 6});
  REQUIRE(transport->Send(moved) > 0);
  REQUIRE(transport->Receive(received, 1000));
  CHECK(received == moved);
}

TEST_CASE("Host interfaces skip loopback", "[net][transport]") {
  for (const auto& host : HostInterfaces()) {
    CHECK(host.address >> 24 != 127);
  }
}

TEST_CASE("Host interfaces are found by name", "[net][transport]") {
  const auto names = HostInterfaceNames();
  for (const auto& host : HostInterfaces()) {
    CHECK_FALSE(host.name.empty());
    CHECK(std::ranges::count(names, host.name) == 1);
    const auto found = FindHostInterface(host.name);
    REQUIRE(found);
    CHECK(found->name == host.name);
  }
  CHECK_FALSE(FindHostInterface("no such interface"));
}

TEST_CASE("Subnet broadcasts skip loopback", "[net][transport]") {
  for (uint32_t address : SubnetBroadcastAddresses()) {
    CHECK(address >> 24 != 127);
    CHECK(address != 0);
    CHECK(address != 0xFFFFFFFF);
  }
}

TEST_CASE("System Links reach each other over the UDP tunnel",
          "[net][transport]") {
  const uint16_t port_a = FreePort();
  uint16_t port_b = FreePort();
  while (port_b == port_a) {
    port_b = FreePort();
  }
  std::string error;
  auto tunnel_a =
      OpenUdpTunnelTransport(kMacA, port_a, {{kLoopback, port_b}}, error);
  auto tunnel_b =
      OpenUdpTunnelTransport(kMacB, port_b, {{kLoopback, port_a}}, error);
  REQUIRE(tunnel_a);
  REQUIRE(tunnel_b);
  const KeyId id{1, 3, 5, 7, 9, 11, 13, 15};
  const Key key{2, 4, 6, 8, 10, 12, 14, 16};
  auto broadcast = DeriveBroadcast(key, [&](auto input, auto& digest) {
    digest = HmacSha1(key, {input});
    return true;
  });
  REQUIRE(broadcast);
  AssociationConfig association;
  association.broadcast = *broadcast;
  SystemLink a(0xC0000201, std::move(tunnel_a), association);
  SystemLink b(0xC0000202, std::move(tunnel_b), association);
  REQUIRE(a.RegisterKey(id, key) == 0);
  REQUIRE(b.RegisterKey(id, key) == 0);
  uint32_t code = 0;
  auto tx = a.Bind(1000, false, code);
  auto rx = b.Bind(1000, false, code);
  REQUIRE(tx);
  REQUIRE(rx);

  const std::vector<uint8_t> data{10, 20, 30, 40};
  const int size = static_cast<int>(data.size());
  std::array<uint8_t, 16> buffer{};
  Endpoint source;
  REQUIRE(tx->Send({0xFFFFFFFF, 1000}, data, code) == size);
  REQUIRE(rx->Receive(buffer, false, source, 2000, code) == size);
  CHECK(source == Endpoint{1, 1000});

  uint32_t address;
  REQUIRE(a.Map(b.identity(), id, address) == 0);
  REQUIRE(tx->Send({address, 1000}, data, code) == size);
  REQUIRE(rx->Receive(buffer, false, source, 5000, code) == size);
  CHECK(std::equal(data.begin(), data.end(), buffer.begin()));
  CHECK(a.ConnectStatus(address) == 2);
}

TEST_CASE("XLink Kai transport speaks DDS", "[net][transport]") {
  Peer engine;
  std::string error;
  auto kai = OpenKaiTransport(kMacA, {kLoopback, engine.port()}, error);
  REQUIRE(kai);
  const std::string identifier = "xenia_021122334401";

  std::vector<uint8_t> received;
  CHECK_FALSE(kai->Receive(received, 50));
  auto connect = engine.Receive(1000);
  REQUIRE(connect);
  CHECK(connect->starts_with("connect;" + identifier + ";Xenia;"));
  const auto client = engine.last_sender;

  // Nothing passes before the engine accepts.
  const auto frame = MakeFrame(kMacA, 3);
  CHECK(kai->Send(frame) == static_cast<int>(frame.size()));
  CHECK_FALSE(engine.Receive(50));
  const auto incoming = MakeFrame(kMacB, 5);
  engine.SendTo("e;e;" + Text(incoming), client);
  CHECK_FALSE(kai->Receive(received, 50));

  engine.SendTo("connected;" + identifier + ";", client);
  CHECK_FALSE(kai->Receive(received, 50));
  REQUIRE(kai->Send(frame) == static_cast<int>(frame.size()));
  auto sent = engine.Receive(1000);
  REQUIRE(sent);
  CHECK(*sent == "e;e;" + Text(frame));

  engine.SendTo("e;e;" + Text(incoming), client);
  REQUIRE(kai->Receive(received, 1000));
  CHECK(received == incoming);

  engine.SendTo("keepalive;", client);
  CHECK_FALSE(kai->Receive(received, 50));
  auto keepalive = engine.Receive(1000);
  REQUIRE(keepalive);
  CHECK(*keepalive == "keepalive;");

  // Only the engine is listened to.
  Peer stranger;
  stranger.SendTo("e;e;" + Text(incoming), client);
  CHECK_FALSE(kai->Receive(received, 50));

  CHECK(kai->healthy());
  engine.SendTo("disconnected;" + identifier + ";bye;", client);
  CHECK_FALSE(kai->Receive(received, 50));
  CHECK_FALSE(kai->healthy());

  kai.reset();
  auto disconnect = engine.Receive(1000);
  REQUIRE(disconnect);
  CHECK(*disconnect == "disconnect;" + identifier + ";");
}

}  // namespace xe::net::secure::test
