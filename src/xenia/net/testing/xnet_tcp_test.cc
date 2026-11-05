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
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "xenia/net/xnet_secure.h"
#include "xenia/net/xnet_tcp.h"

#include "third_party/catch/include/catch.hpp"
#include "third_party/mbedtls/include/mbedtls/des.h"

namespace xe::net::tcp::test {
namespace {

using secure::Kind;
using secure::Packet;
using secure::ProtectionKeys;

ProtectionKeys MakeKeys() {
  ProtectionKeys keys;
  for (size_t i = 0; i < keys.authentication.size(); ++i) {
    keys.authentication[i] = uint8_t(0x10 + i);
  }
  for (size_t i = 0; i < keys.cipher.size(); ++i) {
    keys.cipher[i] = uint8_t(0x31 + 2 * i);
  }
  keys.iv_seed = {1, 2, 3, 4, 5, 6, 7, 8};
  return keys;
}

// Protected packets use single DES with the first eight key bytes.
std::vector<uint8_t> Decrypt(const ProtectionKeys& keys, uint32_t sequence,
                             std::span<const uint8_t> input) {
  std::vector<uint8_t> output(input.size());
  auto iv = secure::PacketIv(keys.iv_seed, sequence);
  mbedtls_des_context context;
  mbedtls_des_init(&context);
  mbedtls_des_setkey_dec(&context, keys.cipher.data());
  mbedtls_des_crypt_cbc(&context, MBEDTLS_DES_DECRYPT, input.size(), iv.data(),
                        input.data(), output.data());
  mbedtls_des_free(&context);
  return output;
}

std::vector<uint8_t> Bytes(size_t size, uint8_t seed) {
  std::vector<uint8_t> bytes(size);
  for (size_t i = 0; i < size; ++i) {
    bytes[i] = uint8_t(seed + i * 7);
  }
  return bytes;
}

// Two connections and the segments in flight between them.
struct Wire {
  uint64_t now = 1000;
  std::optional<Connection> ends[2];
  std::deque<Segment> flight[2];
  // Decides whether a segment from side |from| is lost.
  std::function<bool(unsigned from, const Segment&)> lose;
  // Delivers the segment at the back of the queue first.
  bool reorder = false;
  // A closed end answers with a reset, as the System Link does for segments
  // no connection takes.
  bool refuse = false;
  unsigned delivered = 0;

  void Collect() {
    for (unsigned side : {0u, 1u}) {
      if (!ends[side]) {
        continue;
      }
      for (auto& segment : ends[side]->TakeOutput()) {
        if (lose && lose(side, segment)) {
          continue;
        }
        flight[side].push_back(std::move(segment));
      }
    }
  }

  // Delivers one segment, or advances time when none is in flight.
  void Step() {
    Collect();
    for (unsigned side : {0u, 1u}) {
      if (flight[side].empty()) {
        continue;
      }
      Segment segment;
      if (reorder && flight[side].size() > 1 && delivered % 5 == 0) {
        segment = std::move(flight[side].back());
        flight[side].pop_back();
      } else {
        segment = std::move(flight[side].front());
        flight[side].pop_front();
      }
      ++delivered;
      if (ends[1 - side] && refuse && ends[1 - side]->closed()) {
        if (auto reset = Refusal(segment)) {
          flight[1 - side].push_back(*reset);
        }
      } else if (ends[1 - side]) {
        ends[1 - side]->Input(segment, now);
      }
      Collect();
      return;
    }
    now += 50;
    for (auto& end : ends) {
      if (end) {
        end->Tick(now);
      }
    }
    Collect();
  }

  bool RunUntil(const std::function<bool()>& done, unsigned steps = 200000) {
    for (unsigned i = 0; i < steps; ++i) {
      if (done()) {
        return true;
      }
      Step();
    }
    return done();
  }

  // Opens from side 0 and answers on side 1.
  void Connect() {
    ends[0] = Connection::Open(now);
    auto syn = ends[0]->TakeOutput();
    REQUIRE(syn.size() == 1);
    CHECK(syn[0].flags == kSyn);
    CHECK(syn[0].mss == kSecureMss);
    ends[1] = Connection::Answer(syn[0], now);
    REQUIRE(RunUntil([&] {
      return ends[0]->state() == State::Established &&
             ends[1]->state() == State::Established;
    }));
  }
};

// Moves |data| from one side to the other, reading as it goes.
std::vector<uint8_t> Transfer(Wire& wire, unsigned from,
                              std::span<const uint8_t> data) {
  std::vector<uint8_t> received;
  size_t written = 0;
  std::array<uint8_t, 4096> buffer;
  wire.RunUntil([&] {
    written += wire.ends[from]->Write(data.subspan(written), wire.now);
    const size_t size = wire.ends[1 - from]->Read(buffer, false, wire.now);
    received.insert(received.end(), buffer.begin(), buffer.begin() + size);
    return received.size() == data.size();
  });
  return received;
}

}  // namespace

TEST_CASE("TCP segments carry their header after the ports", "[net][tcp]") {
  const auto keys = MakeKeys();
  struct Case {
    uint16_t source, destination;
    uint8_t kind;
    std::vector<uint8_t> ports;
  };
  const Case cases[] = {
      {1000, 1000, 1, {}},
      {1003, 1250, 2, {3, 250}},
      {28001, 1003, 3, {0x6D, 0x61, 0x03, 0xEB}},
  };
  for (const auto& test : cases) {
    for (size_t body : {0u, 3u, 4u, 8u, 13u}) {
      Packet packet;
      packet.kind = Kind::Tcp;
      packet.source_port = test.source;
      packet.destination_port = test.destination;
      packet.tcp.sequence = 0x01020304;
      packet.tcp.acknowledgment = 0x05060708;
      packet.tcp.flags = kAck | kPsh;
      packet.tcp.window = 0xFEDC;
      packet.encrypted = Bytes(body, 0x40);
      // Four of the bytes are options once there are enough of them.
      packet.tcp.offset = uint8_t(body >= 4 ? 0x60 : 0x50);
      std::vector<uint8_t> wire;
      REQUIRE(secure::Encode(keys, 0x123456, 77, packet, wire));

      std::vector<uint8_t> header = test.ports;
      const std::vector<uint8_t> fields{0x01,
                                        0x02,
                                        0x03,
                                        0x04,
                                        0x05,
                                        0x06,
                                        0x07,
                                        0x08,
                                        packet.tcp.offset,
                                        uint8_t(kAck | kPsh),
                                        0xFE,
                                        0xDC};
      header.insert(header.end(), fields.begin(), fields.end());
      const size_t reserved = (8 - body % 8) % 8;
      const size_t embedded = std::min(reserved, header.size());
      const size_t ciphertext = body + reserved;
      CHECK((wire[0] & 0x0F) == test.kind);
      CHECK(size_t(wire[0] >> 5) == reserved);
      CHECK(wire[1] == 0x12);
      CHECK(wire[2] == 0x34);
      CHECK(wire[3] == 0x56);
      REQUIRE(wire.size() == 4 + ciphertext + header.size() - embedded + 12);
      // The padding takes the first header bytes, the rest follow in the
      // clear, just before the sequence number.
      const auto plain =
          Decrypt(keys, 77, std::span(wire).subspan(4, ciphertext));
      CHECK(std::equal(plain.begin(), plain.begin() + body,
                       packet.encrypted.begin()));
      CHECK(std::all_of(plain.begin() + body, plain.end() - embedded,
                        [](uint8_t byte) { return byte == 0; }));
      CHECK(std::equal(plain.end() - embedded, plain.end(), header.begin()));
      CHECK(std::equal(header.begin() + embedded, header.end(),
                       wire.begin() + 4 + ciphertext));
      CHECK(wire[wire.size() - 12] == 77);
      CHECK(wire[wire.size() - 11] == 0);

      const auto decoded = secure::Decode(keys, 0x123456, 77, wire);
      REQUIRE(decoded);
      CHECK(decoded->kind == Kind::Tcp);
      CHECK(decoded->source_port == test.source);
      CHECK(decoded->destination_port == test.destination);
      CHECK(decoded->tcp == packet.tcp);
      CHECK(decoded->encrypted == packet.encrypted);
      CHECK(decoded->clear.empty());
    }
  }
}

TEST_CASE("TCP header lengths must fit what was encrypted", "[net][tcp]") {
  const auto keys = MakeKeys();
  Packet packet;
  packet.kind = Kind::Tcp;
  packet.encrypted = Bytes(4, 1);
  std::vector<uint8_t> wire;
  for (uint8_t offset : {0x40, 0x70}) {
    packet.tcp.offset = offset;
    CHECK_FALSE(secure::Encode(keys, 9, 1, packet, wire));
  }
  packet.tcp.offset = 0x60;
  REQUIRE(secure::Encode(keys, 9, 1, packet, wire));
  REQUIRE(secure::Decode(keys, 9, 1, wire));
  // Clear data has no place in a segment.
  packet.clear = {1};
  CHECK_FALSE(secure::Encode(keys, 9, 1, packet, wire));
}

TEST_CASE("TCP options give the MSS and stay out of the data", "[net][tcp]") {
  Packet packet;
  packet.kind = Kind::Tcp;
  packet.tcp.flags = kSyn | kAck;
  packet.tcp.offset = 0x80;
  // A SYN with MSS, window scale and SACK permitted options.
  packet.encrypted = {2, 4, 0x04, 0xF0, 1, 3, 3, 0, 1, 1, 4, 2, 9, 8};
  auto segment = FromPacket(packet);
  REQUIRE(segment);
  CHECK(segment->mss == 1264);
  CHECK(segment->data == std::vector<uint8_t>{9, 8});
  // Only a SYN carries one.
  packet.tcp.flags = kAck;
  segment = FromPacket(packet);
  REQUIRE(segment);
  CHECK(segment->mss == 0);

  Segment syn;
  syn.sequence = 5;
  syn.flags = kSyn;
  syn.window = 100;
  syn.mss = kSecureMss;
  syn.data = {7};
  const auto encoded = ToPacket(syn, 1001, 1002);
  CHECK(encoded.kind == Kind::Tcp);
  CHECK(encoded.source_port == 1001);
  CHECK(encoded.destination_port == 1002);
  CHECK(encoded.tcp.offset == 0x60);
  CHECK(encoded.encrypted == std::vector<uint8_t>{2, 4, 0x04, 0xF0, 7});
  const auto back = FromPacket(encoded);
  REQUIRE(back);
  CHECK(back->mss == kSecureMss);
  CHECK(back->data == syn.data);
  CHECK(back->window == 100);
}

TEST_CASE("TCP refuses segments with a reset", "[net][tcp]") {
  Segment syn;
  syn.sequence = 100;
  syn.flags = kSyn;
  auto reset = Refusal(syn);
  REQUIRE(reset);
  CHECK(reset->flags == (kRst | kAck));
  CHECK(reset->acknowledgment == 101);
  Segment data;
  data.sequence = 7;
  data.acknowledgment = 900;
  data.flags = kAck;
  data.data = {1, 2};
  reset = Refusal(data);
  REQUIRE(reset);
  CHECK(reset->flags == kRst);
  CHECK(reset->sequence == 900);
  data.flags = kRst;
  CHECK_FALSE(Refusal(data));
}

TEST_CASE("TCP connections carry data both ways and close", "[net][tcp]") {
  Wire wire;
  wire.Connect();
  CHECK(wire.ends[0]->connected());
  CHECK(wire.ends[1]->connected());
  const auto request = Bytes(3000, 1);
  CHECK(Transfer(wire, 0, request) == request);
  const auto reply = Bytes(5, 9);
  CHECK(Transfer(wire, 1, reply) == reply);

  // Half closed: the side that finished still reads.
  wire.ends[0]->Finish(wire.now);
  REQUIRE(wire.RunUntil([&] {
    return wire.ends[1]->finished_receiving() &&
           wire.ends[0]->state() == State::FinWait2;
  }));
  CHECK(wire.ends[1]->state() == State::CloseWait);
  CHECK(wire.ends[0]->writable() == 0);
  const auto late = Bytes(100, 3);
  CHECK(Transfer(wire, 1, late) == late);
  wire.ends[1]->Finish(wire.now);
  REQUIRE(wire.RunUntil([&] {
    return wire.ends[0]->state() == State::TimeWait && wire.ends[1]->closed();
  }));
  CHECK(wire.ends[0]->finished_receiving());
  CHECK(wire.ends[1]->failure() == Failure::None);
  REQUIRE(wire.RunUntil([&] { return wire.ends[0]->closed(); }));
  CHECK(wire.ends[0]->failure() == Failure::None);
}

TEST_CASE("TCP recovers lost and reordered segments", "[net][tcp]") {
  Wire wire;
  unsigned count = 0;
  wire.lose = [&](unsigned, const Segment&) { return ++count % 6 == 0; };
  wire.reorder = true;
  wire.Connect();
  const auto data = Bytes(200000, 5);
  CHECK(Transfer(wire, 0, data) == data);
  const auto back = Bytes(70000, 11);
  CHECK(Transfer(wire, 1, back) == back);
  wire.ends[0]->Finish(wire.now);
  wire.ends[1]->Finish(wire.now);
  REQUIRE(wire.RunUntil(
      [&] { return wire.ends[0]->closed() && wire.ends[1]->closed(); }));
  CHECK(wire.ends[0]->failure() == Failure::None);
  CHECK(wire.ends[1]->failure() == Failure::None);
}

TEST_CASE("TCP waits for a closed window to open", "[net][tcp]") {
  Wire wire;
  wire.Connect();
  // Twice what the receiver buffers, which it doesn't read yet.
  const auto data = Bytes(2 * kBufferSize, 3);
  size_t written = 0;
  REQUIRE(wire.RunUntil([&] {
    written += wire.ends[0]->Write(std::span(data).subspan(written), wire.now);
    return wire.ends[1]->readable() == kBufferSize;
  }));
  // The window update after a read can go missing, probes find the window.
  bool lose_updates = true;
  wire.lose = [&](unsigned from, const Segment& segment) {
    return lose_updates && from == 1 && segment.window;
  };
  std::vector<uint8_t> received(kBufferSize);
  CHECK(wire.ends[1]->Read(received, false, wire.now) == kBufferSize);
  wire.Collect();
  lose_updates = false;
  std::array<uint8_t, 4096> buffer;
  REQUIRE(wire.RunUntil([&] {
    written += wire.ends[0]->Write(std::span(data).subspan(written), wire.now);
    const size_t size = wire.ends[1]->Read(buffer, false, wire.now);
    received.insert(received.end(), buffer.begin(), buffer.begin() + size);
    return received.size() == data.size();
  }));
  CHECK(received == data);
  CHECK(wire.ends[0]->failure() == Failure::None);
}

TEST_CASE("TCP connects fail when refused or unanswered", "[net][tcp]") {
  uint64_t now = 0;
  auto refused = Connection::Open(now);
  auto syn = refused.TakeOutput();
  REQUIRE(syn.size() == 1);
  auto reset = Refusal(syn[0]);
  REQUIRE(reset);
  refused.Input(*reset, now);
  CHECK(refused.closed());
  CHECK(refused.failure() == Failure::Refused);
  CHECK_FALSE(refused.connected());

  auto unanswered = Connection::Open(now);
  unsigned syns = 0;
  for (; now < 120000 && !unanswered.closed(); now += 50) {
    unanswered.Tick(now);
    for (const auto& segment : unanswered.TakeOutput()) {
      CHECK(segment.flags == kSyn);
      ++syns;
    }
  }
  CHECK(unanswered.failure() == Failure::TimedOut);
  CHECK(syns == 6);
  CHECK(now > 20000);
}

TEST_CASE("TCP connections end on a reset or silence", "[net][tcp]") {
  Wire wire;
  wire.Connect();
  wire.ends[1]->Abort();
  CHECK(wire.ends[1]->closed());
  CHECK(wire.ends[1]->failure() == Failure::None);
  REQUIRE(wire.RunUntil([&] { return wire.ends[0]->closed(); }));
  CHECK(wire.ends[0]->failure() == Failure::Reset);
  CHECK(wire.ends[0]->writable() == 0);

  Wire silent;
  silent.Connect();
  silent.lose = [](unsigned from, const Segment&) { return from == 1; };
  const auto data = Bytes(10, 1);
  CHECK(silent.ends[0]->Write(data, silent.now) == data.size());
  REQUIRE(silent.RunUntil([&] { return silent.ends[0]->closed(); }));
  CHECK(silent.ends[0]->failure() == Failure::TimedOut);
}

TEST_CASE("TCP answers a stray segment of an old connection", "[net][tcp]") {
  Wire wire;
  wire.Connect();
  // The receiver forgot the connection, as after a restart.
  wire.ends[1].reset();
  const auto data = Bytes(10, 1);
  wire.ends[0]->Write(data, wire.now);
  auto sent = wire.ends[0]->TakeOutput();
  REQUIRE(sent.size() == 1);
  auto reset = Refusal(sent[0]);
  REQUIRE(reset);
  wire.ends[0]->Input(*reset, wire.now);
  CHECK(wire.ends[0]->failure() == Failure::Reset);
}

TEST_CASE("TCP sends what was written while connecting", "[net][tcp]") {
  Wire wire;
  wire.ends[0] = Connection::Open(wire.now);
  CHECK(wire.ends[0]->connecting());
  const auto data = Bytes(2000, 4);
  CHECK(wire.ends[0]->Write(data, wire.now) == data.size());
  auto syn = wire.ends[0]->TakeOutput();
  REQUIRE(syn.size() == 1);
  wire.ends[1] = Connection::Answer(syn[0], wire.now);
  std::vector<uint8_t> received;
  std::array<uint8_t, 4096> buffer;
  REQUIRE(wire.RunUntil([&] {
    const size_t size = wire.ends[1]->Read(buffer, false, wire.now);
    received.insert(received.end(), buffer.begin(), buffer.begin() + size);
    return received.size() == data.size();
  }));
  CHECK(received == data);
}

TEST_CASE("TCP acknowledges inside a closed window while it probes",
          "[net][tcp]") {
  SECTION("Both windows closed") {
    Wire wire;
    wire.Connect();
    const auto data = Bytes(2 * kBufferSize, 3);
    size_t written[2] = {};
    REQUIRE(wire.RunUntil([&] {
      for (unsigned side : {0u, 1u}) {
        written[side] += wire.ends[side]->Write(
            std::span(data).subspan(written[side]), wire.now);
      }
      return wire.ends[0]->readable() == kBufferSize &&
             wire.ends[1]->readable() == kBufferSize;
    }));
    // Acknowledgments answering each other would keep time from passing.
    const uint64_t start = wire.now;
    CHECK(wire.RunUntil([&] { return wire.now >= start + 30000; }));
    CHECK(wire.ends[0]->failure() == Failure::None);
    CHECK(wire.ends[1]->failure() == Failure::None);
  }

  SECTION("Data toward the probing side") {
    Wire wire;
    wire.Connect();
    const auto data = Bytes(2 * kBufferSize, 3);
    size_t written = 0;
    REQUIRE(wire.RunUntil([&] {
      written +=
          wire.ends[0]->Write(std::span(data).subspan(written), wire.now);
      return wire.ends[1]->readable() == kBufferSize;
    }));
    // Side 0 probes the closed window by now.
    const uint64_t probing = wire.now;
    wire.RunUntil([&] { return wire.now >= probing + 3000; });
    const auto back = Bytes(20000, 9);
    size_t sent = 0;
    std::vector<uint8_t> received;
    std::array<uint8_t, 4096> buffer;
    const uint64_t start = wire.now;
    REQUIRE(wire.RunUntil([&] {
      sent += wire.ends[1]->Write(std::span(back).subspan(sent), wire.now);
      const size_t size = wire.ends[0]->Read(buffer, false, wire.now);
      received.insert(received.end(), buffer.begin(), buffer.begin() + size);
      return sent == back.size() && wire.ends[1]->writable() == kBufferSize;
    }));
    CHECK(received == back);
    // Side 0's acknowledgments reach side 1 right away.
    CHECK(wire.now - start < 1000);
  }

  SECTION("The window opens after probes") {
    Wire wire;
    wire.Connect();
    const auto data = Bytes(kBufferSize + 100, 3);
    size_t written = 0;
    REQUIRE(wire.RunUntil([&] {
      written +=
          wire.ends[0]->Write(std::span(data).subspan(written), wire.now);
      return wire.ends[1]->readable() == kBufferSize;
    }));
    unsigned probes = 0;
    wire.lose = [&](unsigned from, const Segment& segment) {
      probes += from == 0 && segment.data.size() == 1;
      return false;
    };
    REQUIRE(wire.RunUntil([&] {
      return probes >= 5 && wire.flight[0].empty() && wire.flight[1].empty();
    }));
    std::vector<uint8_t> received(kBufferSize);
    REQUIRE(wire.ends[1]->Read(received, false, wire.now) == kBufferSize);
    const uint64_t start = wire.now;
    std::array<uint8_t, 4096> buffer;
    REQUIRE(wire.RunUntil([&] {
      const size_t size = wire.ends[1]->Read(buffer, false, wire.now);
      received.insert(received.end(), buffer.begin(), buffer.begin() + size);
      return received.size() == data.size();
    }));
    CHECK(received == data);
    // The window update starts the rest, probe byte included.
    CHECK(wire.now - start < 500);
  }
}

TEST_CASE("TCP resets reach the peer after a timeout resend", "[net][tcp]") {
  Wire wire;
  wire.Connect();
  const auto data = Bytes(5000, 1);
  bool lose_acks = true;
  unsigned sends = 0;
  wire.lose = [&](unsigned from, const Segment& segment) {
    sends += from == 0 && !segment.data.empty();
    return lose_acks && from == 1;
  };
  wire.ends[0]->Write(data, wire.now);
  // Side 1 has it all, side 0 resends from what it thinks was lost.
  REQUIRE(wire.RunUntil([&] {
    return wire.ends[1]->readable() == data.size() && sends >= 6 &&
           wire.flight[0].empty();
  }));
  lose_acks = false;
  wire.ends[0]->Abort();
  REQUIRE(wire.RunUntil([&] { return wire.ends[1]->closed(); }, 1000));
  CHECK(wire.ends[1]->failure() == Failure::Reset);
}

TEST_CASE("TCP waits out the peer's resent FIN after closing", "[net][tcp]") {
  Wire wire;
  wire.refuse = true;
  wire.Connect();
  wire.ends[0]->Finish(wire.now);
  REQUIRE(wire.RunUntil([&] {
    return wire.ends[0]->state() == State::FinWait2 &&
           wire.ends[1]->state() == State::CloseWait;
  }));
  // The acknowledgments of side 1's FIN go missing until its resends back off
  // past several seconds.
  unsigned lost = 0;
  wire.lose = [&](unsigned from, const Segment& segment) {
    if (from == 0 && (segment.flags & kAck) &&
        wire.ends[1]->state() == State::LastAck && lost < 5) {
      ++lost;
      return true;
    }
    return false;
  };
  wire.ends[1]->Finish(wire.now);
  REQUIRE(wire.RunUntil([&] { return wire.ends[1]->closed(); }));
  CHECK(lost == 5);
  CHECK(wire.ends[1]->failure() == Failure::None);
}

TEST_CASE("TCP takes a FIN at the edge of a closed window", "[net][tcp]") {
  Wire wire;
  wire.Connect();
  const auto data = Bytes(kBufferSize, 1);
  size_t written = 0;
  REQUIRE(wire.RunUntil([&] {
    written += wire.ends[0]->Write(std::span(data).subspan(written), wire.now);
    return wire.ends[1]->readable() == kBufferSize;
  }));
  wire.ends[0]->Finish(wire.now);
  // Nothing is read, yet the end of the stream is known.
  const uint64_t start = wire.now;
  REQUIRE(wire.RunUntil([&] { return wire.ends[1]->finished_receiving(); }));
  CHECK(wire.now - start < 500);
}

TEST_CASE("TCP keeps a FIN that arrives before the last data", "[net][tcp]") {
  Wire wire;
  wire.Connect();
  // Two segments with the first lost once so the resend doesn't carry the FIN.
  const auto data = Bytes(2000, 1);
  bool lost = false;
  wire.lose = [&](unsigned from, const Segment& segment) {
    if (from == 0 && !segment.data.empty() && !lost) {
      lost = true;
      return true;
    }
    return false;
  };
  wire.ends[0]->Write(data, wire.now);
  wire.ends[0]->Finish(wire.now);
  REQUIRE(
      wire.RunUntil([&] { return wire.ends[1]->readable() == data.size(); }));
  // The FIN came early and waited for the resent data.
  CHECK(wire.ends[1]->finished_receiving());
}

TEST_CASE("TCP resends across the sequence number wrap", "[net][tcp]") {
  Wire wire;
  // The FIN takes the last sequence number before the wrap.
  const auto data = Bytes(2000, 6);
  wire.ends[0] = Connection::Open(wire.now, 0xFFFFFFFFu - 1 - 2000);
  auto syn = wire.ends[0]->TakeOutput();
  REQUIRE(syn.size() == 1);
  wire.ends[1] = Connection::Answer(syn[0], wire.now);
  REQUIRE(wire.RunUntil(
      [&] { return wire.ends[0]->state() == State::Established; }));
  // Everything after the first segment goes missing once.
  unsigned sent = 0;
  wire.lose = [&](unsigned from, const Segment& segment) {
    return from == 0 && segment.length() && ++sent >= 2 && sent <= 3;
  };
  wire.ends[0]->Write(data, wire.now);
  wire.ends[0]->Finish(wire.now);
  std::vector<uint8_t> received;
  std::array<uint8_t, 4096> buffer;
  REQUIRE(wire.RunUntil([&] {
    const size_t size = wire.ends[1]->Read(buffer, false, wire.now);
    received.insert(received.end(), buffer.begin(), buffer.begin() + size);
    return wire.ends[1]->finished_receiving() || received.size() > 2000;
  }));
  CHECK(received == data);
  CHECK(wire.ends[1]->finished_receiving());
}

TEST_CASE("TCP keeps its pace after probing a closed window", "[net][tcp]") {
  Wire wire;
  wire.Connect();
  const auto data = Bytes(2 * kBufferSize, 3);
  size_t written = 0;
  REQUIRE(wire.RunUntil([&] {
    written += wire.ends[0]->Write(std::span(data).subspan(written), wire.now);
    return wire.ends[1]->readable() == kBufferSize;
  }));
  unsigned probes = 0;
  wire.lose = [&](unsigned from, const Segment& segment) {
    probes += from == 0 && segment.data.size() == 1;
    return false;
  };
  REQUIRE(wire.RunUntil([&] {
    return probes >= 5 && wire.flight[0].empty() && wire.flight[1].empty();
  }));
  // The window update reopens the window for a burst.
  std::vector<uint8_t> received(kBufferSize);
  REQUIRE(wire.ends[1]->Read(received, false, wire.now) == kBufferSize);
  wire.Collect();
  REQUIRE(wire.flight[1].size() == 1);
  wire.Step();
  CHECK(wire.flight[0].size() >= 4);
}

TEST_CASE("TCP resets a peer whose window is closed", "[net][tcp]") {
  Wire wire;
  wire.Connect();
  const auto data = Bytes(kBufferSize + 100, 3);
  size_t written = 0;
  REQUIRE(wire.RunUntil([&] {
    written += wire.ends[0]->Write(std::span(data).subspan(written), wire.now);
    return wire.ends[1]->readable() == kBufferSize;
  }));
  unsigned probes = 0;
  wire.lose = [&](unsigned from, const Segment& segment) {
    probes += from == 0 && segment.data.size() == 1;
    return false;
  };
  REQUIRE(wire.RunUntil([&] {
    return probes >= 1 && wire.flight[0].empty() && wire.flight[1].empty();
  }));
  wire.ends[0]->Abort();
  REQUIRE(wire.RunUntil([&] { return wire.ends[1]->closed(); }, 1000));
  CHECK(wire.ends[1]->failure() == Failure::Reset);
}

TEST_CASE("TCP reports a simultaneous open that is reset as refused",
          "[net][tcp]") {
  uint64_t now = 0;
  auto a = Connection::Open(now);
  auto b = Connection::Open(now);
  const auto a_syn = a.TakeOutput();
  const auto b_syn = b.TakeOutput();
  REQUIRE(a_syn.size() == 1);
  REQUIRE(b_syn.size() == 1);
  a.Input(b_syn[0], now);
  CHECK(a.state() == State::SynReceived);
  const auto syn_ack = a.TakeOutput();
  REQUIRE(syn_ack.size() == 1);
  // The peer forgot its side and resets the SYN-ACK.
  auto reset = Refusal(syn_ack[0]);
  REQUIRE(reset);
  a.Input(*reset, now);
  CHECK(a.closed());
  CHECK(a.failure() == Failure::Refused);
}

}  // namespace xe::net::tcp::test
