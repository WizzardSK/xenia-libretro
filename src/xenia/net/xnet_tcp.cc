/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/net/xnet_tcp.h"

#include <algorithm>
#include <array>
#include <utility>

namespace xe::net::tcp {
namespace {

// Local timing policy.
constexpr uint64_t kInitialRtoMs = 1000, kMinRtoMs = 200, kMaxRtoMs = 8000;
constexpr unsigned kSynRetries = 5, kRetries = 10;
// Twice a segment lifetime, past the peer's slowest resend of its FIN.
constexpr uint64_t kTimeWaitMs = 60000;
constexpr unsigned kDuplicateAcks = 3;
constexpr size_t kEarlyLimit = 64;
// What a peer that sends no MSS option takes.
constexpr uint16_t kDefaultMss = 536;
constexpr uint8_t kEndOption = 0, kNopOption = 1, kMssOption = 2;

// Sequence numbers compare modulo 2^32.
bool Before(uint32_t a, uint32_t b) { return int32_t(a - b) < 0; }
bool AtOrBefore(uint32_t a, uint32_t b) { return int32_t(a - b) <= 0; }

uint32_t InitialSequence() {
  std::array<uint8_t, 4> bytes;
  Random(bytes);
  return uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 |
         uint32_t(bytes[2]) << 8 | bytes[3];
}

}  // namespace

uint32_t Segment::length() const {
  return uint32_t(data.size()) + ((flags & kSyn) ? 1 : 0) +
         ((flags & kFin) ? 1 : 0);
}

std::optional<Segment> FromPacket(const secure::Packet& packet) {
  const size_t words = packet.tcp.offset >> 4;
  if (packet.kind != secure::Kind::Tcp || words < 5 ||
      (words - 5) * 4 > packet.encrypted.size()) {
    return std::nullopt;
  }
  Segment segment;
  segment.sequence = packet.tcp.sequence;
  segment.acknowledgment = packet.tcp.acknowledgment;
  segment.flags = packet.tcp.flags;
  segment.window = packet.tcp.window;
  const size_t options_size = (words - 5) * 4;
  auto options = std::span(packet.encrypted).first(options_size);
  // Parsing stops at anything malformed, as the rest can't be trusted.
  while (!options.empty() && options[0] != kEndOption) {
    if (options[0] == kNopOption) {
      options = options.subspan(1);
      continue;
    }
    if (options.size() < 2 || options[1] < 2 || options[1] > options.size()) {
      break;
    }
    if (options[0] == kMssOption && options[1] == 4 && (segment.flags & kSyn)) {
      segment.mss = uint16_t(options[2] << 8 | options[3]);
    }
    options = options.subspan(options[1]);
  }
  segment.data.assign(packet.encrypted.begin() + options_size,
                      packet.encrypted.end());
  return segment;
}

secure::Packet ToPacket(const Segment& segment, uint16_t source,
                        uint16_t destination) {
  secure::Packet packet;
  packet.kind = secure::Kind::Tcp;
  packet.source_port = source;
  packet.destination_port = destination;
  packet.tcp.sequence = segment.sequence;
  packet.tcp.acknowledgment = segment.acknowledgment;
  packet.tcp.flags = segment.flags;
  packet.tcp.window = segment.window;
  if (segment.mss) {
    packet.encrypted = {kMssOption, 4, uint8_t(segment.mss >> 8),
                        uint8_t(segment.mss)};
  }
  packet.tcp.offset = uint8_t((5 + packet.encrypted.size() / 4) << 4);
  packet.encrypted.insert(packet.encrypted.end(), segment.data.begin(),
                          segment.data.end());
  return packet;
}

std::optional<Segment> Refusal(const Segment& segment) {
  if (segment.flags & kRst) {
    return std::nullopt;
  }
  Segment reset;
  if (segment.flags & kAck) {
    reset.sequence = segment.acknowledgment;
    reset.flags = kRst;
  } else {
    reset.acknowledgment = segment.sequence + segment.length();
    reset.flags = kRst | kAck;
  }
  return reset;
}

Connection Connection::Open(uint64_t now) {
  return Open(now, InitialSequence());
}

Connection Connection::Open(uint64_t now, uint32_t iss) {
  Connection connection;
  connection.state_ = State::SynSent;
  connection.active_ = true;
  connection.iss_ = iss;
  connection.snd_una_ = connection.iss_;
  connection.snd_nxt_ = connection.snd_max_ = connection.iss_ + 1;
  connection.send_sequence_ = connection.iss_ + 1;
  connection.rto_ = kInitialRtoMs;
  connection.Emit(connection.iss_, kSyn);
  connection.timing_ = true;
  connection.timed_sequence_ = connection.snd_nxt_;
  connection.timed_at_ = now;
  connection.retransmit_at_ = now + connection.rto_;
  return connection;
}

Connection Connection::Answer(const Segment& syn, uint64_t now) {
  Connection connection;
  connection.state_ = State::SynReceived;
  connection.irs_ = syn.sequence;
  connection.rcv_nxt_ = syn.sequence + 1;
  connection.peer_mss_ = syn.mss ? syn.mss : kDefaultMss;
  connection.iss_ = InitialSequence();
  connection.snd_una_ = connection.iss_;
  connection.snd_nxt_ = connection.snd_max_ = connection.iss_ + 1;
  connection.send_sequence_ = connection.iss_ + 1;
  connection.rto_ = kInitialRtoMs;
  connection.Emit(connection.iss_, kSyn | kAck);
  connection.timing_ = true;
  connection.timed_sequence_ = connection.snd_nxt_;
  connection.timed_at_ = now;
  connection.retransmit_at_ = now + connection.rto_;
  return connection;
}

size_t Connection::writable() const {
  if ((state_ != State::Established && state_ != State::CloseWait &&
       !connecting()) ||
      fin_queued_) {
    return 0;
  }
  return kBufferSize - send_.size();
}

bool Connection::Synchronized() const {
  return state_ != State::Closed && !connecting();
}

uint16_t Connection::Window() const {
  return uint16_t(kBufferSize - receive_.size());
}

uint32_t Connection::SendMss() const {
  return std::min<uint32_t>(peer_mss_ ? peer_mss_ : kDefaultMss, kSecureMss);
}

uint32_t Connection::DataEnd() const {
  return send_sequence_ + uint32_t(send_.size());
}

bool Connection::FinAcknowledged() const {
  return fin_queued_ && snd_una_ == DataEnd() + 1;
}

void Connection::Emit(uint32_t sequence, uint8_t flags,
                      std::span<const uint8_t> data) {
  Segment segment;
  segment.sequence = sequence;
  segment.flags = flags;
  segment.data.assign(data.begin(), data.end());
  if (flags & kSyn) {
    segment.mss = kSecureMss;
  }
  // A bare reset carries no window.
  if (flags != kRst) {
    segment.window = Window();
  }
  if (flags & kAck) {
    segment.acknowledgment = rcv_nxt_;
    advertised_ = rcv_nxt_ + segment.window;
    ack_now_ = false;
  }
  output_.push_back(std::move(segment));
}

void Connection::Input(const Segment& segment, uint64_t now) {
  if (state_ == State::Closed) {
    return;
  }
  if (state_ == State::SynSent) {
    InputSynSent(segment, now);
    Output(now);
    return;
  }
  // The peer resent the SYN this connection answers.
  if (state_ == State::SynReceived && (segment.flags & kSyn) &&
      !(segment.flags & kAck) && segment.sequence == irs_) {
    Emit(iss_, kSyn | kAck);
    return;
  }

  Segment trimmed = segment;
  const uint32_t window = Window();
  // Drop what arrived already but keep the acknowledgment.
  if (Before(trimmed.sequence, rcv_nxt_)) {
    const bool repeat = trimmed.length() && !(trimmed.flags & kRst);
    uint32_t old = rcv_nxt_ - trimmed.sequence;
    if (trimmed.flags & kSyn) {
      trimmed.flags = uint8_t(trimmed.flags & ~kSyn);
      ++trimmed.sequence;
      --old;
    }
    if (old >= trimmed.data.size()) {
      if (old > trimmed.data.size()) {
        trimmed.flags = uint8_t(trimmed.flags & ~kFin);
      }
      // Nothing new in it so the peer missed an acknowledgment.
      if (repeat && !(trimmed.flags & kFin)) {
        ack_now_ = true;
      }
      trimmed.data.clear();
    } else {
      trimmed.data.erase(trimmed.data.begin(), trimmed.data.begin() + old);
    }
    trimmed.sequence = rcv_nxt_;
  }
  // And what lies past the window. An empty segment or a bare FIN fits a
  // closed window at its edge.
  const uint32_t offset = trimmed.sequence - rcv_nxt_;
  const uint32_t occupied =
      uint32_t(trimmed.data.size()) + ((trimmed.flags & kSyn) ? 1 : 0);
  if (offset >= window && (occupied || offset)) {
    if (!(trimmed.flags & kRst)) {
      ack_now_ = true;
      Output(now);
    }
    // A closed window still takes the acknowledgment at its edge.
    if (offset || !(trimmed.flags & kAck)) {
      return;
    }
    trimmed.data.clear();
    trimmed.flags = uint8_t(trimmed.flags & ~(kFin | kSyn));
  } else if (offset + trimmed.data.size() > window) {
    trimmed.data.resize(window - offset);
    trimmed.flags = uint8_t(trimmed.flags & ~kFin);
  }

  if (trimmed.flags & kRst) {
    // Only a reset that starts in the window counts.
    if (!Before(segment.sequence, rcv_nxt_)) {
      // A handshake this side never finished just goes away, unless it was
      // this side's own connect.
      Fail(connected_ ? Failure::Reset
           : active_  ? Failure::Refused
                      : Failure::None);
    }
    return;
  }
  if (trimmed.flags & kSyn) {
    // A synchronized connection takes no new SYN, the acknowledgment tells
    // the sender where it stands.
    ack_now_ = true;
    Output(now);
    return;
  }
  if (!(trimmed.flags & kAck)) {
    return;
  }
  if (state_ == State::SynReceived) {
    if (AtOrBefore(trimmed.acknowledgment, snd_una_) ||
        Before(snd_max_, trimmed.acknowledgment)) {
      Emit(trimmed.acknowledgment, kRst);
      return;
    }
    Establish(trimmed);
  }
  if (!Acknowledge(trimmed, now)) {
    Output(now);
    return;
  }
  if (FinAcknowledged()) {
    if (state_ == State::FinWait1) {
      state_ = State::FinWait2;
    } else if (state_ == State::Closing) {
      state_ = State::TimeWait;
      time_wait_until_ = now + kTimeWaitMs;
    } else if (state_ == State::LastAck) {
      state_ = State::Closed;
      return;
    }
  }
  if (state_ == State::Established || state_ == State::FinWait1 ||
      state_ == State::FinWait2) {
    Accept(trimmed, now);
  } else if ((segment.flags & kFin) && state_ == State::TimeWait) {
    // The peer missed the acknowledgment of its FIN.
    ack_now_ = true;
    time_wait_until_ = now + kTimeWaitMs;
  }
  Output(now);
}

void Connection::InputSynSent(const Segment& segment, uint64_t now) {
  if (segment.flags & kAck) {
    if (AtOrBefore(segment.acknowledgment, iss_) ||
        Before(snd_max_, segment.acknowledgment)) {
      if (!(segment.flags & kRst)) {
        Emit(segment.acknowledgment, kRst);
      }
      return;
    }
  }
  if (segment.flags & kRst) {
    if (segment.flags & kAck) {
      Fail(Failure::Refused);
    }
    return;
  }
  if (!(segment.flags & kSyn)) {
    return;
  }
  irs_ = segment.sequence;
  rcv_nxt_ = segment.sequence + 1;
  peer_mss_ = segment.mss ? segment.mss : kDefaultMss;
  if (!(segment.flags & kAck)) {
    // Both ends opened at once.
    state_ = State::SynReceived;
    Emit(iss_, kSyn | kAck);
    return;
  }
  Establish(segment);
  Acknowledge(segment, now);
  ack_now_ = true;
}

void Connection::Establish(const Segment& segment) {
  state_ = State::Established;
  connected_ = true;
  snd_wnd_ = segment.window;
  snd_wl1_ = segment.sequence;
  snd_wl2_ = segment.acknowledgment;
  cwnd_ = 4 * SendMss();
  ssthresh_ = uint32_t(kBufferSize);
  retries_ = 0;
  retransmit_at_ = 0;
}

bool Connection::Acknowledge(const Segment& segment, uint64_t now) {
  const uint32_t ack = segment.acknowledgment;
  if (Before(snd_max_, ack)) {
    ack_now_ = true;
    return false;
  }
  if (Before(snd_una_, ack)) {
    if (timing_ && AtOrBefore(timed_sequence_, ack)) {
      Sample(now - timed_at_);
      timing_ = false;
    }
    if (Before(send_sequence_, ack)) {
      const size_t acked = std::min<size_t>(ack - send_sequence_, send_.size());
      send_.erase(send_.begin(), send_.begin() + acked);
      send_sequence_ += uint32_t(acked);
    }
    snd_una_ = ack;
    // What went out before a timeout arrived after all.
    if (Before(snd_nxt_, ack)) {
      snd_nxt_ = ack;
    }
    duplicate_acks_ = 0;
    retries_ = 0;
    const uint32_t mss = SendMss();
    cwnd_ += cwnd_ < ssthresh_ ? mss : std::max<uint32_t>(1, mss * mss / cwnd_);
    cwnd_ = std::min<uint32_t>(cwnd_, uint32_t(2 * kBufferSize));
    retransmit_at_ = snd_una_ == snd_max_ ? 0 : now + rto_;
  } else if (ack == snd_una_ && snd_una_ != snd_max_ && segment.window &&
             segment.data.empty() && !(segment.flags & (kSyn | kFin)) &&
             segment.window == snd_wnd_) {
    // The third duplicate means the segment after |ack| went missing.
    if (++duplicate_acks_ == kDuplicateAcks) {
      ssthresh_ = std::max((snd_max_ - snd_una_) / 2, 2 * SendMss());
      cwnd_ = ssthresh_;
      timing_ = false;
      Resend(now);
    }
  }
  // A peer that keeps answering window probes is still there.
  if (!segment.window) {
    retries_ = 0;
  }
  if (Before(snd_wl1_, segment.sequence) ||
      (snd_wl1_ == segment.sequence && AtOrBefore(snd_wl2_, ack))) {
    snd_wnd_ = segment.window;
    snd_wl1_ = segment.sequence;
    snd_wl2_ = ack;
  }
  // Only probes go past a closed window and it takes acknowledgments only from
  // its edge. Sending resumes there.
  if (!snd_wnd_ && Before(snd_una_, DataEnd())) {
    snd_nxt_ = snd_una_;
  }
  return true;
}

void Connection::Accept(const Segment& segment, uint64_t now) {
  if (segment.sequence != rcv_nxt_) {
    if (!segment.data.empty() || (segment.flags & kFin)) {
      // Kept until the gap fills. The duplicate acknowledgment tells the peer
      // about the gap.
      ack_now_ = true;
      if (early_.size() < kEarlyLimit) {
        early_.push_back(
            {segment.sequence, segment.data, (segment.flags & kFin) != 0});
      }
    }
    return;
  }
  if (!segment.data.empty()) {
    receive_.insert(receive_.end(), segment.data.begin(), segment.data.end());
    rcv_nxt_ += uint32_t(segment.data.size());
    ack_now_ = true;
  }
  bool fin = (segment.flags & kFin) != 0;
  for (bool progress = true; progress && !fin;) {
    progress = false;
    for (auto early = early_.begin(); early != early_.end(); ++early) {
      if (Before(rcv_nxt_, early->sequence)) {
        continue;
      }
      const uint32_t old = rcv_nxt_ - early->sequence;
      if (old < early->data.size()) {
        const size_t size =
            std::min(early->data.size() - old, kBufferSize - receive_.size());
        receive_.insert(receive_.end(), early->data.begin() + old,
                        early->data.begin() + old + size);
        rcv_nxt_ += uint32_t(size);
      }
      // Its FIN counts only once every byte before it is in.
      fin = early->fin &&
            rcv_nxt_ == early->sequence + uint32_t(early->data.size());
      early_.erase(early);
      progress = true;
      break;
    }
  }
  if (fin) {
    early_.clear();
    ReceiveFin(now);
  }
}

void Connection::ReceiveFin(uint64_t now) {
  ++rcv_nxt_;
  fin_received_ = true;
  ack_now_ = true;
  switch (state_) {
    case State::Established:
      state_ = State::CloseWait;
      break;
    case State::FinWait1:
      if (FinAcknowledged()) {
        state_ = State::TimeWait;
        time_wait_until_ = now + kTimeWaitMs;
      } else {
        state_ = State::Closing;
      }
      break;
    case State::FinWait2:
      state_ = State::TimeWait;
      time_wait_until_ = now + kTimeWaitMs;
      break;
    default:
      break;
  }
}

void Connection::Sample(uint64_t rtt) {
  // RFC 6298 with a 1 ms clock.
  if (!have_rtt_) {
    srtt_ = rtt;
    rttvar_ = rtt / 2;
    have_rtt_ = true;
  } else {
    const uint64_t delta = srtt_ > rtt ? srtt_ - rtt : rtt - srtt_;
    rttvar_ = (3 * rttvar_ + delta) / 4;
    srtt_ = (7 * srtt_ + rtt) / 8;
  }
  rto_ = std::clamp(srtt_ + std::max<uint64_t>(1, 4 * rttvar_), kMinRtoMs,
                    kMaxRtoMs);
}

uint32_t Connection::Resend(uint64_t now) {
  const uint32_t end = DataEnd();
  const uint32_t unacknowledged =
      Before(snd_una_, end)
          ? (Before(end, snd_max_) ? end : snd_max_) - snd_una_
          : 0;
  const uint32_t size = std::min(unacknowledged, SendMss());
  std::vector<uint8_t> data;
  if (size) {
    const size_t first = snd_una_ - send_sequence_;
    data.assign(send_.begin() + first, send_.begin() + first + size);
  }
  uint8_t flags = kAck;
  // FIN went out after these bytes.
  if (fin_queued_ && snd_una_ + size == end && Before(end, snd_max_)) {
    flags |= kFin;
  }
  if (!size && !(flags & kFin)) {
    return 0;
  }
  Emit(snd_una_, flags, data);
  if (!retransmit_at_) {
    retransmit_at_ = now + rto_;
  }
  return size + ((flags & kFin) ? 1 : 0);
}

void Connection::Output(uint64_t now) {
  if (state_ == State::Established || state_ == State::CloseWait ||
      state_ == State::FinWait1 || state_ == State::Closing ||
      state_ == State::LastAck) {
    const uint32_t end = DataEnd();
    while (Before(snd_nxt_, end)) {
      const uint32_t unsent = end - snd_nxt_;
      const uint32_t limit = std::min(snd_wnd_, cwnd_);
      const uint32_t flight = snd_nxt_ - snd_una_;
      if (limit <= flight) {
        break;
      }
      const uint32_t size = std::min({unsent, limit - flight, SendMss()});
      const size_t first = snd_nxt_ - send_sequence_;
      std::vector<uint8_t> data(send_.begin() + first,
                                send_.begin() + first + size);
      Emit(snd_nxt_, uint8_t(kAck | (size == unsent ? kPsh : 0)), data);
      if (!timing_ && snd_nxt_ == snd_max_) {
        timing_ = true;
        timed_sequence_ = snd_nxt_ + size;
        timed_at_ = now;
      }
      snd_nxt_ += size;
      if (Before(snd_max_, snd_nxt_)) {
        snd_max_ = snd_nxt_;
      }
      if (!retransmit_at_) {
        retransmit_at_ = now + rto_;
      }
    }
    if (fin_queued_ && snd_nxt_ == end) {
      Emit(snd_nxt_, kFin | kAck);
      ++snd_nxt_;
      if (Before(snd_max_, snd_nxt_)) {
        snd_max_ = snd_nxt_;
      }
      if (state_ == State::Established) {
        state_ = State::FinWait1;
      } else if (state_ == State::CloseWait) {
        state_ = State::LastAck;
      }
      if (!retransmit_at_) {
        retransmit_at_ = now + rto_;
      }
    }
    // Probes reopen a closed window.
    if (!snd_wnd_ && Before(snd_una_, end) && !retransmit_at_) {
      retransmit_at_ = now + rto_;
    }
  }
  if (ack_now_ && Synchronized()) {
    Emit(snd_nxt_, kAck);
  }
}

void Connection::Tick(uint64_t now) {
  if (state_ == State::TimeWait) {
    if (now >= time_wait_until_) {
      state_ = State::Closed;
    }
    return;
  }
  if (!retransmit_at_ || now < retransmit_at_) {
    return;
  }
  timing_ = false;
  if (connecting()) {
    if (++retries_ > kSynRetries) {
      Fail(Failure::TimedOut);
      return;
    }
    Emit(iss_, state_ == State::SynSent ? kSyn : uint8_t(kSyn | kAck));
  } else if (!snd_wnd_ && Before(snd_una_, DataEnd())) {
    // The first byte past the closed window, which the peer may take. The
    // next one to send stays at the window's edge.
    if (++retries_ > kRetries) {
      Fail(Failure::TimedOut);
      return;
    }
    const uint8_t probe = send_[snd_una_ - send_sequence_];
    Emit(snd_una_, kAck, std::span(&probe, 1));
    snd_nxt_ = snd_una_;
    if (Before(snd_max_, snd_una_ + 1)) {
      snd_max_ = snd_una_ + 1;
    }
  } else if (snd_una_ != snd_max_) {
    if (++retries_ > kRetries) {
      Fail(Failure::TimedOut);
      return;
    }
    ssthresh_ = std::max((snd_max_ - snd_una_) / 2, 2 * SendMss());
    cwnd_ = SendMss();
    duplicate_acks_ = 0;
    // Everything after the lost segment goes again as acknowledgments
    // arrive.
    snd_nxt_ = snd_una_ + Resend(now);
  } else {
    // Nothing is outstanding any more.
    retransmit_at_ = 0;
    return;
  }
  rto_ = std::min(rto_ * 2, kMaxRtoMs);
  retransmit_at_ = now + rto_;
}

size_t Connection::Write(std::span<const uint8_t> data, uint64_t now) {
  const size_t size = std::min(data.size(), writable());
  send_.insert(send_.end(), data.begin(), data.begin() + size);
  Output(now);
  return size;
}

size_t Connection::Read(std::span<uint8_t> buffer, bool peek, uint64_t now) {
  const size_t size = std::min(buffer.size(), receive_.size());
  std::copy_n(receive_.begin(), size, buffer.begin());
  if (peek || !size) {
    return size;
  }
  receive_.erase(receive_.begin(), receive_.begin() + size);
  // Tells the peer about room it can use, as RFC 1122 has it.
  const uint32_t grown = rcv_nxt_ + Window() - advertised_;
  if (Synchronized() && !fin_received_ &&
      grown >= std::min<uint32_t>(kSecureMss, kBufferSize / 2)) {
    ack_now_ = true;
    Output(now);
  }
  return size;
}

void Connection::Finish(uint64_t now) {
  if (fin_queued_ || state_ == State::Closed || state_ == State::SynSent) {
    return;
  }
  fin_queued_ = true;
  Output(now);
}

void Connection::Abort() {
  if (state_ == State::Closed) {
    return;
  }
  // The peer may hold everything sent, even after a timeout went back to
  // resend. A closed window takes it only at its edge.
  if (state_ != State::SynSent) {
    Emit(snd_wnd_ ? snd_max_ : snd_nxt_, kRst);
  }
  Fail(Failure::None);
}

void Connection::Detach() {
  if (state_ == State::Closed) {
    return;
  }
  if (fin_received_) {
    auto received = std::move(receive_);
    Fail(Failure::None);
    receive_ = std::move(received);
  } else {
    Fail(Failure::Reset);
  }
  output_.clear();
}

void Connection::Fail(Failure failure) {
  state_ = State::Closed;
  failure_ = failure;
  retransmit_at_ = 0;
  timing_ = false;
  send_.clear();
  receive_.clear();
  early_.clear();
}

std::vector<Segment> Connection::TakeOutput() {
  return std::exchange(output_, {});
}

}  // namespace xe::net::tcp
