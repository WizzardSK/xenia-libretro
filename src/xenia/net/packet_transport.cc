/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/net/packet_transport.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string_view>

#include <asio.hpp>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/clock.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"

#if !XE_PLATFORM_WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

namespace xe::net::secure {
namespace {

using udp = asio::ip::udp;

constexpr uint32_t kMtu = 1500;
constexpr size_t kEthernetHeaderSize = 14;
constexpr uint32_t kLimitedBroadcast = 0xFFFFFFFF;

uint64_t Now() { return Clock::QueryHostUptimeMillis(); }

udp::endpoint ToAsio(UdpEndpoint endpoint) {
  return {asio::ip::address_v4(endpoint.address), endpoint.port};
}

// Windows reports an ICMP error for an earlier send as a failed receive.
// Nothing is wrong with the socket.
bool Transient(const asio::error_code& ec) {
#if XE_PLATFORM_WIN32
  // Overlapped receives give the unreachables as Win32 codes.
  if (ec.category() == asio::system_category() &&
      (ec.value() == ERROR_NETWORK_UNREACHABLE ||
       ec.value() == ERROR_HOST_UNREACHABLE)) {
    return true;
  }
#endif
  return ec == asio::error::connection_reset ||
         ec == asio::error::connection_refused ||
         ec == asio::error::network_reset ||
         ec == asio::error::network_unreachable ||
         ec == asio::error::host_unreachable;
}

class UdpSocket {
 public:
  UdpSocket() : socket_(io_) {}

  bool Open(uint32_t address, uint16_t port, bool shared, std::string& error) {
    asio::error_code ec;
    socket_.open(udp::v4(), ec);
    if (!ec && shared) {
      socket_.set_option(udp::socket::reuse_address(true), ec);
    }
    if (!ec) {
      socket_.set_option(asio::socket_base::broadcast(true), ec);
    }
    if (!ec) {
      socket_.bind({asio::ip::address_v4(address), port}, ec);
    }
    if (ec) {
      error = fmt::format("UDP port {}: {}", port, ec.message());
      return false;
    }
    return true;
  }

  asio::error_code SendTo(std::span<const uint8_t> data,
                          const udp::endpoint& destination) {
    asio::error_code ec;
    socket_.send_to(asio::buffer(data.data(), data.size()), destination, 0, ec);
    return ec;
  }

  // Waits up to |timeout_ms| for one datagram. Empty on timeout.
  std::optional<asio::error_code> ReceiveFor(std::span<const uint8_t>& data,
                                             udp::endpoint& sender,
                                             uint32_t timeout_ms) {
    std::optional<asio::error_code> result;
    size_t size = 0;
    socket_.async_receive_from(
        asio::buffer(buffer_), sender,
        [&](const asio::error_code& ec, size_t received) {
          result = ec;
          size = received;
        });
    io_.restart();
    io_.run_for(std::chrono::milliseconds(timeout_ms));
    if (!result) {
      socket_.cancel();
      io_.run();
    }
    if (*result == asio::error::operation_aborted) {
      return std::nullopt;
    }
    data = std::span(buffer_.data(), size);
    return result;
  }

  // Cancels the ReceiveFor running on another thread, or the next one.
  void Wake() {
    woken_ = true;
    asio::post(io_, [this] {
      asio::error_code ec;
      socket_.cancel(ec);
    });
  }

  // Whether Wake() was called since the last call.
  bool TakeWake() { return woken_.exchange(false); }

  udp::socket::native_handle_type native_handle() {
    return socket_.native_handle();
  }

 private:
  asio::io_context io_;
  udp::socket socket_;
  std::vector<uint8_t> buffer_ = std::vector<uint8_t>(65536);
  std::atomic<bool> woken_{false};
};

// https://www.teamxlink.co.uk/wiki/Emulator_Integration_Protocol
class KaiTransport final : public PacketTransport {
 public:
  KaiTransport(const Mac& mac, UdpEndpoint engine)
      : mac_(mac),
        engine_(ToAsio(engine)),
        identifier_(fmt::format("xenia_{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
                                mac[0], mac[1], mac[2], mac[3], mac[4],
                                mac[5])) {}

  ~KaiTransport() override {
    Command(fmt::format("disconnect;{};", identifier_));
  }

  bool Open(std::string& error) {
    // A local engine needs no port open on the LAN.
    const auto address = engine_.address().is_loopback()
                             ? asio::ip::address_v4::loopback()
                             : asio::ip::address_v4::any();
    return socket_.Open(address.to_uint(), 0, false, error);
  }

  Mac identity() const override { return mac_; }
  uint32_t mtu() const override { return kMtu; }
  bool healthy() const override { return healthy_; }

  int Send(std::span<const uint8_t> frame) override {
    // Without a connection the frame goes nowhere, as on a LAN with no link.
    if (!connected_) {
      return static_cast<int>(frame.size());
    }
    out_.assign(kFrameTag.begin(), kFrameTag.end());
    out_.insert(out_.end(), frame.begin(), frame.end());
    if (auto ec = socket_.SendTo(out_, engine_); ec && !Transient(ec)) {
      return -1;
    }
    return static_cast<int>(frame.size());
  }

  bool Receive(std::vector<uint8_t>& frame, uint32_t timeout_ms) override {
    const uint64_t deadline = Now() + timeout_ms;
    while (healthy_) {
      if (socket_.TakeWake()) {
        return false;
      }
      const uint64_t now = Now();
      if (!connected_ && now >= next_connect_) {
        // The engine may start after us, so keep asking.
        Command(
            fmt::format("connect;{};Xenia;0;0;000000000000000", identifier_));
        if (!next_connect_) {
          XELOGI("XLink Kai: waiting for the engine at {}:{}",
                 engine_.address().to_string(), engine_.port());
        }
        next_connect_ = now + 5000;
      }
      if (now >= deadline) {
        return false;
      }
      const uint64_t until =
          connected_ ? deadline : std::min(deadline, next_connect_);
      std::span<const uint8_t> data;
      udp::endpoint sender;
      auto ec = socket_.ReceiveFor(data, sender, uint32_t(until - now));
      if (!ec || Transient(*ec)) {
        continue;
      }
      if (*ec) {
        XELOGE("XLink Kai: receive failed: {}", ec->message());
        healthy_ = false;
        break;
      }
      if (sender != engine_) {
        continue;
      }
      if (data.size() > kFrameTag.size() &&
          std::equal(kFrameTag.begin(), kFrameTag.end(), data.begin())) {
        if (connected_) {
          frame.assign(data.begin() + kFrameTag.size(), data.end());
          return true;
        }
        continue;
      }
      Control(std::string_view(reinterpret_cast<const char*>(data.data()),
                               data.size()));
    }
    return false;
  }

  void Wake() override { socket_.Wake(); }

 private:
  static constexpr std::string_view kFrameTag = "e;e;";

  void Command(std::string_view command) {
    socket_.SendTo(
        {reinterpret_cast<const uint8_t*>(command.data()), command.size()},
        engine_);
  }

  void Control(std::string_view message) {
    if (message.starts_with("connected;")) {
      if (!connected_) {
        XELOGI("XLink Kai: connected");
      }
      connected_ = true;
    } else if (message.starts_with("disconnected;")) {
      XELOGE("XLink Kai: disconnected: {}", message);
      connected_ = false;
      healthy_ = false;
    } else if (message.starts_with("keepalive;")) {
      Command("keepalive;");
    } else if (message.starts_with("message;")) {
      XELOGI("XLink Kai: {}", message.substr(8));
    }
  }

  Mac mac_;
  udp::endpoint engine_;
  std::string identifier_;
  UdpSocket socket_;
  std::vector<uint8_t> out_;
  bool connected_ = false;
  uint64_t next_connect_ = 0;
  std::atomic<bool> healthy_{true};
};

class UdpTunnelTransport final : public PacketTransport {
 public:
  UdpTunnelTransport(const Mac& mac, std::vector<UdpEndpoint> destinations)
      : mac_(mac), destinations_(std::move(destinations)) {}

  bool Open(uint16_t port, std::string& error) {
    return socket_.Open(0, port, true, error);
  }

  Mac identity() const override { return mac_; }
  uint32_t mtu() const override { return kMtu; }
  bool healthy() const override { return healthy_; }

  int Send(std::span<const uint8_t> frame) override {
    RefreshTargets();
    out_.assign(kTag.begin(), kTag.end());
    out_.insert(out_.end(), frame.begin(), frame.end());
    if (frame.size() >= kEthernetHeaderSize) {
      Mac destination;
      std::copy_n(frame.begin(), destination.size(), destination.begin());
      if (auto peer = Direct(destination)) {
        auto ec = socket_.SendTo(out_, *peer);
        // A peer that can't be reached directly still gets the broadcast.
        if (!ec || Transient(ec)) {
          return static_cast<int>(frame.size());
        }
      }
    }
    bool sent = false;
    for (const auto& target : targets_) {
      auto ec = socket_.SendTo(out_, target);
      sent |= !ec || Transient(ec);
    }
    return sent ? static_cast<int>(frame.size()) : -1;
  }

  bool Receive(std::vector<uint8_t>& frame, uint32_t timeout_ms) override {
    const uint64_t deadline = Now() + timeout_ms;
    while (healthy_) {
      if (socket_.TakeWake()) {
        return false;
      }
      const uint64_t now = Now();
      if (now >= deadline) {
        return false;
      }
      std::span<const uint8_t> data;
      udp::endpoint sender;
      auto ec = socket_.ReceiveFor(data, sender, uint32_t(deadline - now));
      if (!ec || Transient(*ec)) {
        continue;
      }
      if (*ec) {
        XELOGE("System Link tunnel: receive failed: {}", ec->message());
        healthy_ = false;
        break;
      }
      auto payload = data.subspan(std::min(data.size(), kTag.size()));
      if (data.size() < kTag.size() + kEthernetHeaderSize ||
          !std::equal(kTag.begin(), kTag.end(), data.begin())) {
        continue;
      }
      Mac source;
      std::copy_n(payload.begin() + 6, source.size(), source.begin());
      // A group address never sends.
      if (source[0] & 1) {
        continue;
      }
      // Broadcasts come back to the sender too, from its own addresses.
      if (source == mac_) {
        if (local_.size() < kPeerLimit) {
          local_.insert(sender);
        }
        continue;
      }
      auto peer = peers_.find(source);
      if (peer != peers_.end()) {
        peer->second = {sender, Now()};
      } else if (peers_.size() < kPeerLimit) {
        peers_.emplace(source, Heard{sender, Now()});
        XELOGI(
            "System Link tunnel: hearing "
            "{:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X} from {}",
            source[0], source[1], source[2], source[3], source[4], source[5],
            sender.address().to_string());
      }
      frame.assign(payload.begin(), payload.end());
      return true;
    }
    return false;
  }

  void Wake() override { socket_.Wake(); }

 private:
  static constexpr std::string_view kTag = "XESL";
  static constexpr size_t kPeerLimit = 256;
  // A MAC quiet for this long at an address no longer shares it, which is how
  // a restarted instance's old MAC looks.
  static constexpr uint64_t kQuietMs = 3000;

  struct Heard {
    udp::endpoint address;
    uint64_t at_ms = 0;
  };

  // Frames go straight to a peer only at an address no other instance shares
  // since a unicast reaches just one of the instances bound there.
  std::optional<udp::endpoint> Direct(const Mac& destination) const {
    auto peer = peers_.find(destination);
    if ((destination[0] & 1) || peer == peers_.end() ||
        local_.contains(peer->second.address)) {
      return std::nullopt;
    }
    const uint64_t now = Now();
    for (const auto& [mac, heard] : peers_) {
      if (mac != destination && heard.address == peer->second.address &&
          now - heard.at_ms < kQuietMs) {
        return std::nullopt;
      }
    }
    return peer->second.address;
  }

  // Windows sends a limited broadcast out of one interface only, which can be
  // a virtual one. Sending to the broadcast address of each subnet reaches
  // them all. They are looked up again every 5 s since interfaces come and
  // go.
  void RefreshTargets() {
    const uint64_t now = Now();
    if (now < next_refresh_) {
      return;
    }
    next_refresh_ = now + 5000;
    std::vector<udp::endpoint> targets;
    for (const auto& destination : destinations_) {
      if (destination.address != kLimitedBroadcast) {
        targets.push_back(ToAsio(destination));
        continue;
      }
      auto subnets = SubnetBroadcastAddresses();
      if (subnets.empty()) {
        subnets.push_back(kLimitedBroadcast);
      }
      for (uint32_t subnet : subnets) {
        targets.push_back(ToAsio({subnet, destination.port}));
      }
    }
    if (targets != targets_) {
      std::string list;
      for (const auto& target : targets) {
        list += fmt::format("{}{}:{}", list.empty() ? "" : ", ",
                            target.address().to_string(), target.port());
      }
      XELOGI("System Link tunnel: sending to {}", list);
      targets_ = std::move(targets);
    }
  }

  Mac mac_;
  std::vector<UdpEndpoint> destinations_;
  std::vector<udp::endpoint> targets_;
  uint64_t next_refresh_ = 0;
  // Where and when each peer was last heard.
  std::map<Mac, Heard> peers_;
  // The addresses this instance's own broadcasts come back from.
  std::set<udp::endpoint> local_;
  UdpSocket socket_;
  std::vector<uint8_t> out_;
  std::atomic<bool> healthy_{true};
};

class HostTransport final : public PacketTransport {
 public:
  explicit HostTransport(const HostInterface& host) : host_(host) {}

  bool Open(uint16_t port, std::string& error) {
    socket_ = OpenSocket(port, error);
    port_ = port;
    return socket_ != nullptr;
  }

  Mac identity() const override { return host_.mac; }
  uint32_t mtu() const override { return kMtu; }
  bool healthy() const override { return healthy_; }

  int Send(std::span<const uint8_t> frame) override {
    // EncodeFrame puts the IPv4 header right after the Ethernet one and the
    // UDP header after that.
    constexpr size_t kPayloadOffset = kEthernetHeaderSize + 28;
    if (frame.size() < kPayloadOffset) {
      return -1;
    }
    const auto ip = frame.subspan(kEthernetHeaderSize + 16, 4);
    const uint32_t destination = uint32_t(ip[0]) << 24 | uint32_t(ip[1]) << 16 |
                                 uint32_t(ip[2]) << 8 | ip[3];
    auto [socket, port] = Current();
    if (auto ec = socket->SendTo(frame.subspan(kPayloadOffset),
                                 ToAsio({destination, port}));
        ec && !Transient(ec)) {
      return -1;
    }
    return static_cast<int>(frame.size());
  }

  bool Receive(std::vector<uint8_t>& frame, uint32_t timeout_ms) override {
    auto [socket, port] = Current();
    const uint64_t deadline = Now() + timeout_ms;
    while (healthy_) {
      if (socket->TakeWake()) {
        return false;
      }
      const uint64_t now = Now();
      if (now >= deadline) {
        return false;
      }
      std::span<const uint8_t> data;
      udp::endpoint sender;
      auto ec = socket->ReceiveFor(data, sender, uint32_t(deadline - now));
      if (!ec || Transient(*ec)) {
        continue;
      }
      if (*ec) {
        XELOGE("System Link: receive on UDP port {} failed: {}", port,
               ec->message());
        healthy_ = false;
        break;
      }
      // Peers only send from the System Link port.
      if (sender.port() != port) {
        continue;
      }
      // A datagram doesn't tell where it was sent but only broadcasts use
      // the broadcast keys.
      Frame wrapped;
      wrapped.source = IpMachine(sender.address().to_v4().to_uint());
      wrapped.broadcast = BroadcastPayload(data);
      wrapped.destination = wrapped.broadcast
                                ? Mac{255, 255, 255, 255, 255, 255}
                                : IpMachine(host_.address);
      wrapped.payload.assign(data.begin(), data.end());
      auto encoded = EncodeFrame(wrapped, CrossPlatform{.port = port});
      if (!encoded) {
        continue;
      }
      frame = std::move(*encoded);
      return true;
    }
    return false;
  }

  void Wake() override { Current().first->Wake(); }

  bool SetPort(uint16_t port) override {
    std::string error;
    auto socket = OpenSocket(port, error);
    if (!socket) {
      XELOGE("System Link: {}", error);
      return false;
    }
    std::shared_ptr<UdpSocket> previous;
    {
      std::lock_guard lock(mutex_);
      previous = std::exchange(socket_, socket);
      port_ = port;
    }
    // A wake no receive has seen yet still ends the next one.
    const bool woken = previous->TakeWake();
    previous->Wake();
    if (woken) {
      socket->Wake();
    }
    return true;
  }

 private:
  std::pair<std::shared_ptr<UdpSocket>, uint16_t> Current() const {
    std::lock_guard lock(mutex_);
    return {socket_, port_};
  }

  std::shared_ptr<UdpSocket> OpenSocket(uint16_t port,
                                        std::string& error) const {
    auto socket = std::make_shared<UdpSocket>();
#if XE_PLATFORM_WIN32
    // Windows sends limited broadcasts out of the interface a socket is bound
    // to and still hands it the broadcasts that arrive there.
    const uint32_t address = host_.address;
#else
    // Only sockets bound to any address get broadcasts.
    const uint32_t address = 0;
#endif
    if (!socket->Open(address, port, false, error)) {
      return nullptr;
    }
#if XE_PLATFORM_MAC
    const int index = int(host_.index);
    if (setsockopt(socket->native_handle(), IPPROTO_IP, IP_BOUND_IF, &index,
                   sizeof(index))) {
      error = fmt::format("UDP port {}: cannot keep to interface {}", port,
                          host_.index);
      return nullptr;
    }
#elif XE_PLATFORM_LINUX
    // Limited broadcasts follow the default route unless told otherwise. The
    // index goes in network byte order.
    const uint32_t index = htonl(host_.index);
    if (setsockopt(socket->native_handle(), IPPROTO_IP, IP_UNICAST_IF, &index,
                   sizeof(index))) {
      error = fmt::format("UDP port {}: cannot keep to interface {}", port,
                          host_.index);
      return nullptr;
    }
#endif
    return socket;
  }

  HostInterface host_;
  mutable std::mutex mutex_;
  std::shared_ptr<UdpSocket> socket_;
  uint16_t port_ = 0;
  std::atomic<bool> healthy_{true};
};

}  // namespace

std::unique_ptr<PacketTransport> OpenKaiTransport(const Mac& mac,
                                                  UdpEndpoint engine,
                                                  std::string& error) {
  auto transport = std::make_unique<KaiTransport>(mac, engine);
  if (!transport->Open(error)) {
    return nullptr;
  }
  return transport;
}

std::unique_ptr<PacketTransport> OpenUdpTunnelTransport(
    const Mac& mac, uint16_t port, std::vector<UdpEndpoint> destinations,
    std::string& error) {
  auto transport =
      std::make_unique<UdpTunnelTransport>(mac, std::move(destinations));
  if (!transport->Open(port, error)) {
    return nullptr;
  }
  return transport;
}

std::unique_ptr<PacketTransport> OpenHostTransport(const HostInterface& host,
                                                   uint16_t port,
                                                   std::string& error) {
  auto transport = std::make_unique<HostTransport>(host);
  if (!transport->Open(port, error)) {
    return nullptr;
  }
  return transport;
}

}  // namespace xe::net::secure
