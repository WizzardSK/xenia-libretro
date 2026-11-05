/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "xenia/net/xnet_secure.h"

#include "third_party/catch/include/catch.hpp"
#include "third_party/rapidjson/include/rapidjson/document.h"

namespace xe::net::secure::test {
namespace {

std::vector<uint8_t> Hex(const rapidjson::Value& value) {
  std::string text(value.GetString(), value.GetStringLength());
  if (text.starts_with("0x")) {
    text.erase(0, 2);
  }
  REQUIRE(text.size() % 2 == 0);
  std::vector<uint8_t> bytes;
  for (size_t i = 0; i < text.size(); i += 2) {
    bytes.push_back(
        static_cast<uint8_t>(std::stoul(text.substr(i, 2), nullptr, 16)));
  }
  return bytes;
}

template <size_t N>
std::array<uint8_t, N> Fixed(const rapidjson::Value& value) {
  auto bytes = Hex(value);
  REQUIRE(bytes.size() == N);
  std::array<uint8_t, N> result;
  std::copy(bytes.begin(), bytes.end(), result.begin());
  return result;
}

rapidjson::Document LoadVectors() {
  std::ifstream input(XE_XNET_SYNTHETIC_VECTORS);
  REQUIRE(input.good());
  std::stringstream text;
  text << input.rdbuf();
  rapidjson::Document vectors;
  vectors.Parse(text.str().c_str());
  REQUIRE_FALSE(vectors.HasParseError());
  REQUIRE(std::string(vectors["origin"].GetString()) == "synthetic-only");
  return vectors;
}

ProtectionKeys Broadcast(const rapidjson::Document& vectors) {
  const auto platform_key = Fixed<16>(vectors["inputs"]["platform_hmac_key"]);
  auto broadcast =
      DeriveBroadcast(Fixed<16>(vectors["inputs"]["executable_lan_key"]),
                      [&](auto bytes, auto& digest) {
                        digest = HmacSha1(platform_key, {bytes});
                        return true;
                      });
  REQUIRE(broadcast);
  return *broadcast;
}

RegistrationKeys Registration(const rapidjson::Document& vectors) {
  return DeriveRegistration(Broadcast(vectors).authentication,
                            Fixed<8>(vectors["inputs"]["session_id"]),
                            Fixed<16>(vectors["inputs"]["session_key"]));
}

// DES ignores the low bit of each key byte, which odd parity sets.
bool SameDesKey(std::span<const uint8_t> a, std::span<const uint8_t> b) {
  return std::ranges::equal(
      a, b, [](uint8_t x, uint8_t y) { return (x & 0xFE) == (y & 0xFE); });
}

Mac MacOf(const Address& address) {
  Mac mac;
  std::copy_n(address.begin() + 10, mac.size(), mac.begin());
  return mac;
}

Packet PacketFrom(const rapidjson::Value& item) {
  const std::string type = item["type"].GetString();
  Packet packet;
  packet.kind = type == "discovery" ? Kind::Discovery
                : type == "udp"     ? Kind::Udp
                : type == "control" ? Kind::Control
                                    : Kind::Vdp;
  packet.source_port = uint16_t(
      item.HasMember("source_port") ? item["source_port"].GetUint() : 1000);
  packet.destination_port = uint16_t(item.HasMember("destination_port")
                                         ? item["destination_port"].GetUint()
                                         : 1000);
  packet.control_type = uint16_t(
      item.HasMember("control_type") ? item["control_type"].GetUint() : 0);
  packet.encrypted = Hex(item["body"]);
  if (item.HasMember("clear")) {
    packet.clear = Hex(item["clear"]);
  }
  if (packet.kind == Kind::Discovery) {
    packet.metadata = Fixed<12>(item["metadata"]);
  }
  return packet;
}

}  // namespace

TEST_CASE("XNet key derivation matches the synthetic vectors",
          "[net][secure]") {
  const auto vectors = LoadVectors();
  const auto& inputs = vectors["inputs"];
  const auto& derived = vectors["derived"];

  const auto broadcast = Broadcast(vectors);
  const auto broadcast_stream = Hex(derived["broadcast_stream"]);
  CHECK(std::ranges::equal(broadcast.authentication,
                           std::span(broadcast_stream).first(16)));
  CHECK(broadcast.cipher == Fixed<24>(derived["broadcast_cipher_odd_parity"]));
  CHECK(std::ranges::equal(broadcast.iv_seed,
                           std::span(broadcast_stream).subspan(40, 8)));

  const auto registration = Registration(vectors);
  const auto registration_stream = Hex(derived["registration_stream"]);
  CHECK(std::ranges::equal(registration.authentication,
                           std::span(registration_stream).first(16)));
  CHECK(registration.cipher ==
        Fixed<24>(derived["registration_cipher_odd_parity"]));

  const auto request = Hex(vectors["exchange"]["request"]);
  const auto response = Hex(vectors["exchange"]["response"]);
  auto initiator = DhKeyPair::FromPrivate(Hex(inputs["initiator_exponent"]));
  auto responder = DhKeyPair::FromPrivate(Hex(inputs["responder_exponent"]));
  REQUIRE(initiator);
  REQUIRE(responder);
  CHECK(std::ranges::equal(initiator->public_value(),
                           std::span(request).subspan(136, 96)));
  CHECK(std::ranges::equal(responder->public_value(),
                           std::span(response).subspan(136, 96)));
  const auto initiator_shared = initiator->Shared(responder->public_value());
  const auto responder_shared = responder->Shared(initiator->public_value());
  REQUIRE(initiator_shared);
  REQUIRE(responder_shared);
  CHECK(*initiator_shared == *responder_shared);

  Block initiator_challenge, responder_challenge;
  std::copy_n(request.begin() + 28, 8, initiator_challenge.begin());
  std::copy_n(response.begin() + 36, 8, responder_challenge.begin());
  const auto traffic =
      DeriveTraffic(registration.authentication, *initiator_shared,
                    initiator_challenge, responder_challenge);
  const auto stream = Hex(derived["traffic_stream"]);
  REQUIRE(stream.size() == 96);
  CHECK(std::ranges::equal(traffic.a.authentication,
                           std::span(stream).first(16)));
  CHECK(SameDesKey(traffic.a.cipher, std::span(stream).subspan(16, 24)));
  CHECK(std::ranges::equal(traffic.b.authentication,
                           std::span(stream).subspan(40, 16)));
  CHECK(SameDesKey(traffic.b.cipher, std::span(stream).subspan(56, 24)));
  CHECK(
      std::ranges::equal(traffic.a.iv_seed, std::span(stream).subspan(80, 8)));
  CHECK(
      std::ranges::equal(traffic.b.iv_seed, std::span(stream).subspan(88, 8)));
  CHECK(std::ranges::equal(std::span(traffic.a.cipher).first(8),
                           Hex(derived["traffic_cipher_A"])));
}

TEST_CASE("XNet exchange records match the synthetic vectors",
          "[net][secure]") {
  const auto vectors = LoadVectors();
  const auto& inputs = vectors["inputs"];
  const auto& exchange = vectors["exchange"];
  const auto registration = Registration(vectors);
  const auto id = Fixed<8>(inputs["session_id"]);
  const auto initiator_address = Fixed<36>(inputs["initiator_address"]);
  const auto responder_address = Fixed<36>(inputs["responder_address"]);

  const auto request_wire = Hex(exchange["request"]);
  const auto request = DecodeExchange(registration, id, request_wire);
  REQUIRE(request);
  CHECK_FALSE(request->response);
  CHECK(request->initiator_address == initiator_address);
  CHECK(request->responder_address == responder_address);
  CHECK(request->address_iv == Fixed<8>(exchange["initiator_iv"]));
  auto encoded = EncodeExchange(registration, *request);
  REQUIRE(encoded);
  CHECK(std::ranges::equal(*encoded, request_wire));

  const auto response_wire = Hex(exchange["response"]);
  const auto response = DecodeExchange(registration, id, response_wire);
  REQUIRE(response);
  CHECK(response->response);
  CHECK(response->responder_spi == 0x040506);
  CHECK(ResponseMatches(*request, *response));
  encoded = EncodeExchange(registration, *response);
  REQUIRE(encoded);
  CHECK(std::ranges::equal(*encoded, response_wire));

  auto tampered = request_wire;
  tampered.back() ^= 1;
  CHECK_FALSE(DecodeExchange(registration, id, tampered));

  const auto frame_wire = Hex(exchange["request_frame"]);
  Frame frame;
  frame.source = MacOf(initiator_address);
  frame.destination = MacOf(responder_address);
  frame.payload = request_wire;
  const auto frame_encoded = EncodeFrame(frame);
  REQUIRE(frame_encoded);
  CHECK(*frame_encoded == frame_wire);
  const auto frame_decoded = DecodeFrame(frame_wire);
  REQUIRE(frame_decoded);
  CHECK(frame_decoded->payload == request_wire);
}

TEST_CASE("XNet packet codecs match the synthetic vectors", "[net][secure]") {
  const auto vectors = LoadVectors();
  const auto broadcast = Broadcast(vectors);
  const auto stream = Hex(vectors["derived"]["traffic_stream"]);
  ProtectionKeys traffic;
  std::copy_n(stream.begin(), 16, traffic.authentication.begin());
  std::copy_n(stream.begin() + 16, 24, traffic.cipher.begin());
  std::copy_n(stream.begin() + 80, 8, traffic.iv_seed.begin());
  uint32_t traffic_spi = 0;
  for (uint8_t byte : Hex(vectors["packet_destination_spi"])) {
    traffic_spi = (traffic_spi << 8) | byte;
  }

  REQUIRE(vectors["packets"].Size() == 107);
  for (const auto& item : vectors["packets"].GetArray()) {
    INFO(item["name"].GetString());
    const auto packet = PacketFrom(item);
    const bool discovery = packet.kind == Kind::Discovery;
    const auto& keys = discovery ? broadcast : traffic;
    const uint32_t spi = discovery ? 0 : traffic_spi;
    const uint32_t sequence = item["sequence"].GetUint();
    const auto expected = Hex(item["wire"]);
    std::vector<uint8_t> wire;
    REQUIRE(Encode(keys, spi, sequence, packet, wire));
    CHECK(wire == expected);
    auto decoded = Decode(keys, spi, sequence, expected, discovery);
    REQUIRE(decoded);
    CHECK(decoded->encrypted == packet.encrypted);
    CHECK(decoded->clear == packet.clear);
    CHECK(decoded->source_port == packet.source_port);
    CHECK(decoded->destination_port == packet.destination_port);
    CHECK(decoded->metadata == packet.metadata);
    for (size_t i = 0; i < expected.size(); ++i) {
      CHECK_FALSE(
          Decode(keys, spi, sequence, std::span(expected).first(i), discovery));
      auto mutation = expected;
      mutation[i] ^= 1;
      CHECK_FALSE(Decode(keys, spi, sequence, mutation, discovery));
    }
  }

  for (const auto& negative : vectors["negative"].GetArray()) {
    INFO(negative["name"].GetString());
    const std::string based_on = negative["based_on"].GetString();
    const auto& packets = vectors["packets"].GetArray();
    const auto base = std::find_if(
        packets.begin(), packets.end(),
        [&](const auto& item) { return based_on == item["name"].GetString(); });
    REQUIRE(base != packets.end());
    const bool discovery = PacketFrom(*base).kind == Kind::Discovery;
    CHECK_FALSE(Decode(
        discovery ? broadcast : traffic, discovery ? 0 : traffic_spi,
        (*base)["sequence"].GetUint(), Hex(negative["wire"]), discovery));
  }
}

}  // namespace xe::net::secure::test
