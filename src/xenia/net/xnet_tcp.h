/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_NET_XNET_TCP_H_
#define XENIA_NET_XNET_TCP_H_

#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <vector>

#include "xenia/net/xnet_secure.h"

namespace xe::net::tcp {

inline constexpr uint8_t kFin = 0x01;
inline constexpr uint8_t kSyn = 0x02;
inline constexpr uint8_t kRst = 0x04;
inline constexpr uint8_t kPsh = 0x08;
inline constexpr uint8_t kAck = 0x10;

// The MSS for secure peers, which leaves room for the security header in an
// Ethernet frame.
inline constexpr uint16_t kSecureMss = 1264;
// Each direction buffers this much. Windows are never scaled.
inline constexpr size_t kBufferSize = 0xFFFF;

struct Segment {
  uint32_t sequence = 0, acknowledgment = 0;
  uint8_t flags = 0;
  uint16_t window = 0;
  // From the MSS option, 0 without one. No other option is kept.
  uint16_t mss = 0;
  std::vector<uint8_t> data;

  // Sequence space the segment takes, where SYN and FIN count one each.
  uint32_t length() const;
};

std::optional<Segment> FromPacket(const secure::Packet& packet);
secure::Packet ToPacket(const Segment& segment, uint16_t source,
                        uint16_t destination);
// What answers a segment that no connection takes. Nothing answers a reset.
std::optional<Segment> Refusal(const Segment& segment);

enum class State {
  Closed,
  SynSent,
  SynReceived,
  Established,
  FinWait1,
  FinWait2,
  Closing,
  TimeWait,
  CloseWait,
  LastAck,
};

enum class Failure { None, Refused, Reset, TimedOut };

// One end of a connection. The owner serializes calls, routes segments to it
// by ports and peer and sends what TakeOutput() returns.
class Connection {
 public:
  // Sends SYN.
  static Connection Open(uint64_t now_ms);
  static Connection Open(uint64_t now_ms, uint32_t iss);
  // Answers |syn| with SYN-ACK.
  static Connection Answer(const Segment& syn, uint64_t now_ms);

  State state() const { return state_; }
  // Why the connection ended early, if it did.
  Failure failure() const { return failure_; }
  // The handshake finished, even if the connection ended since.
  bool connected() const { return connected_; }
  bool connecting() const {
    return state_ == State::SynSent || state_ == State::SynReceived;
  }
  // The owner can drop a closed connection.
  bool closed() const { return state_ == State::Closed; }
  // Received bytes waiting to be read.
  size_t readable() const { return receive_.size(); }
  // Room left to send, 0 once sending is over.
  size_t writable() const;
  // The peer sent FIN so no more data follows what is readable.
  bool finished_receiving() const { return fin_received_; }

  void Input(const Segment& segment, uint64_t now_ms);
  void Tick(uint64_t now_ms);
  // Queues as much of |data| as the send buffer has room for.
  size_t Write(std::span<const uint8_t> data, uint64_t now_ms);
  size_t Read(std::span<uint8_t> buffer, bool peek, uint64_t now_ms);
  // Sends FIN after the queued data.
  void Finish(uint64_t now_ms);
  // Resets a synchronized peer and closes without a failure.
  void Abort();
  // Closes without a word to the peer, which is gone. What arrived up to its
  // FIN stays readable, without a FIN the connection ends as if reset.
  void Detach();

  std::vector<Segment> TakeOutput();

 private:
  // Out-of-order data waiting for the gap before it to fill.
  struct Early {
    uint32_t sequence = 0;
    std::vector<uint8_t> data;
    bool fin = false;
  };

  Connection() = default;

  bool Synchronized() const;
  uint16_t Window() const;
  uint32_t SendMss() const;
  // Sequence number just past the queued data, where FIN goes.
  uint32_t DataEnd() const;
  bool FinAcknowledged() const;

  void Emit(uint32_t sequence, uint8_t flags,
            std::span<const uint8_t> data = {});
  void InputSynSent(const Segment& segment, uint64_t now_ms);
  void Establish(const Segment& segment);
  // False when the segment acknowledges data never sent.
  bool Acknowledge(const Segment& segment, uint64_t now_ms);
  void Accept(const Segment& segment, uint64_t now_ms);
  void ReceiveFin(uint64_t now_ms);
  void Sample(uint64_t rtt_ms);
  // Sends the first unacknowledged segment even past the window and returns
  // the sequence space it took.
  uint32_t Resend(uint64_t now_ms);
  void Output(uint64_t now_ms);
  void Fail(Failure failure);

  State state_ = State::Closed;
  Failure failure_ = Failure::None;
  bool connected_ = false;
  // Opened by this side rather than answering a SYN.
  bool active_ = false;

  uint32_t iss_ = 0, snd_una_ = 0, snd_nxt_ = 0, snd_wl1_ = 0, snd_wl2_ = 0;
  // The highest sequence number sent, which snd_nxt_ falls behind while a
  // timeout resends or the peer's window is closed.
  uint32_t snd_max_ = 0;
  uint32_t snd_wnd_ = 0;
  // Sequence number of send_.front().
  uint32_t send_sequence_ = 0;
  std::deque<uint8_t> send_;
  bool fin_queued_ = false;
  uint16_t peer_mss_ = 0;
  uint32_t cwnd_ = 0, ssthresh_ = 0;
  unsigned duplicate_acks_ = 0;

  uint32_t rcv_nxt_ = 0, irs_ = 0;
  // Where the last window advertised ends.
  uint32_t advertised_ = 0;
  std::deque<uint8_t> receive_;
  std::vector<Early> early_;
  bool fin_received_ = false;
  bool ack_now_ = false;

  uint64_t rto_ = 0, srtt_ = 0, rttvar_ = 0;
  bool have_rtt_ = false;
  // One segment at a time is timed, never a resent one.
  bool timing_ = false;
  uint32_t timed_sequence_ = 0;
  uint64_t timed_at_ = 0;
  // 0 while no retransmission or window probe is due.
  uint64_t retransmit_at_ = 0;
  unsigned retries_ = 0;
  uint64_t time_wait_until_ = 0;

  std::vector<Segment> output_;
};

}  // namespace xe::net::tcp

#endif  // XENIA_NET_XNET_TCP_H_
