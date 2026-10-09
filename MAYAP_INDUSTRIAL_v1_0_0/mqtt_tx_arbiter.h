#pragma once

#include <stddef.h>
#include <stdint.h>

// Policy of the single MQTT transmit path. Pure C++11, no Arduino types: the host tests (tests/mqtt-tx-arbiter.cpp) run exactly this
// code. mqtt_transport.h owns the socket and answers "may a frame of this lane go out right now?" (one select(), nothing built yet);
// transaction_bridge.h owns the producers and asks BEFORE it builds anything.
//
// Why this exists. The uplink of the device is a small TCP send window shared by everything the firmware says to the Web. Measured
// on a real unit the Web felt slow because the periodic 1-2 KB snapshot kept winning every moment the socket became writable while
// the 300-byte terminal acknowledgement of a command sat in a back-off timer (a priority inversion), and because a PUBACK that did
// not fit in a six-slot queue was dropped without a trace (the broker then waited for it and closed the device with 1013).
//
// The rules (highest first):
//   1. PUBACK owed to the broker      - never dropped; a bounded queue of packet ids, flushed first; overflow = controlled reconnect.
//   2. alarm / heartbeat uplink        - published by the transport itself, ahead of everything the bridge says.
//   3. terminal command acknowledgement - retried on EVERY pass at the first writable moment (no timer back-off); while one waits,
//                                        the lanes below stand aside (bounded by ACK_PRIORITY_MAX_MS).
//   4. bulk reports (config / history) - need a free QoS1 slot beyond the ack reserve and an open wire.
//   5. telemetry (snapshot, log)       - latest wins, paced; a closed wire is asked BEFORE the JSON is built, and congestion
//                                        stretches the interval a little (additive increase / additive decrease).
namespace MayapTx {

enum class Lane : uint8_t { Ack = 0, Bulk = 1, Telemetry = 2 };
constexpr uint8_t LANE_COUNT = 3U;

constexpr uint8_t PUBACK_QUEUE = 32U;                   // the broker sends at most 16 QoS1 packets unacknowledged; 32 covers a stall of its 20 s eviction
constexpr uint32_t ACK_PRIORITY_MAX_MS = 2500UL;        // lanes below the ack stand aside for at most this long
constexpr uint32_t SNAPSHOT_FORCED_MIN_GAP_MS = 150UL;  // a terminal result asks for a fresh snapshot; several results within this window share one
constexpr uint32_t SNAPSHOT_MAX_GAP_MS = 4000UL;        // congestion never stretches the live interval beyond this
constexpr uint32_t SNAPSHOT_GAP_STEP_UP_MS = 500UL;
constexpr uint32_t SNAPSHOT_GAP_STEP_DOWN_MS = 250UL;
constexpr uint32_t SNAPSHOT_FAILED_RETRY_MS = 100UL;    // the wire was open but the publish still failed: do not rebuild the JSON on every pass

// PUBACK debts, in order. push() fails only when the queue is full - the caller must then restart the link instead of pretending.
template <uint8_t N>
class IdQueue {
 public:
  bool push(uint16_t id) {
    if (count_ >= N) { ++rejected_; return false; }
    ids_[static_cast<uint8_t>((head_ + count_) % N)] = id;
    ++count_;
    if (count_ > high_) high_ = count_;
    return true;
  }
  bool empty() const { return count_ == 0U; }
  uint8_t size() const { return count_; }
  uint16_t front() const { return ids_[head_]; }
  void pop() { if (count_ > 0U) { head_ = static_cast<uint8_t>((head_ + 1U) % N); --count_; } }
  void clear() { head_ = 0U; count_ = 0U; }
  uint8_t high() const { return high_; }
  void resetHigh() { high_ = count_; }
  uint32_t rejected() const { return rejected_; }
 private:
  uint16_t ids_[N] = {};
  uint8_t head_ = 0U, count_ = 0U, high_ = 0U;
  uint32_t rejected_ = 0U;
};
typedef IdQueue<PUBACK_QUEUE> PubackQueue;

// How long a producer that wanted to send had to wait for the wire, per lane. note() is called with every answer the gate gives.
class LaneWatch {
 public:
  void note(Lane lane, bool open, uint32_t now) {
    Entry &e = lanes_[static_cast<uint8_t>(lane)];
    if (open) {
      if (e.since != 0U) {
        const uint32_t waited = static_cast<uint32_t>(now - e.since);
        if (waited > e.maxWaitMs) e.maxWaitMs = waited;
        if (waited > e.worstWaitMs) e.worstWaitMs = waited;
        e.since = 0U;
      }
    } else if (e.since == 0U) {
      e.since = now != 0U ? now : 1U;
      ++e.episodes;
    }
  }
  uint32_t episodes(Lane lane) const { return lanes_[static_cast<uint8_t>(lane)].episodes; }
  uint32_t maxWaitMs(Lane lane) const { return lanes_[static_cast<uint8_t>(lane)].maxWaitMs; }     // this diagnostic window
  uint32_t worstWaitMs(Lane lane) const { return lanes_[static_cast<uint8_t>(lane)].worstWaitMs; } // whole uptime
  bool waiting(Lane lane) const { return lanes_[static_cast<uint8_t>(lane)].since != 0U; }
  void resetWindow() { for (Entry &e : lanes_) e.maxWaitMs = 0U; }
  void clearWaiting() { for (Entry &e : lanes_) e.since = 0U; }       // a new link starts with nobody waiting
 private:
  struct Entry { uint32_t since = 0U, episodes = 0U, maxWaitMs = 0U, worstWaitMs = 0U; };
  Entry lanes_[LANE_COUNT];
};

// Pacing of the live snapshot. `extra` is the congestion allowance added to the base interval: it grows by one step each time a
// snapshot was due but the wire was closed (once per episode, however many passes the episode lasts) and shrinks by one step after
// each snapshot that went out without such an episode. A forced snapshot (a command just finished) skips the interval but never
// the minimum gap, so a run of quick commands shares one fresh state instead of producing one 2 KB frame each.
class SnapshotPacer {
 public:
  bool due(uint32_t now, uint32_t lastSentAt, uint32_t baseMs, bool forced) const {
    const uint32_t elapsed = static_cast<uint32_t>(now - lastSentAt);
    if (forced) return elapsed >= SNAPSHOT_FORCED_MIN_GAP_MS;
    return elapsed >= baseMs + allowance(baseMs);
  }
  uint32_t allowance(uint32_t baseMs) const {
    if (baseMs >= SNAPSHOT_MAX_GAP_MS) return 0U;                    // the idle heartbeat is slow enough already
    const uint32_t room = SNAPSHOT_MAX_GAP_MS - baseMs;
    return extra_ < room ? extra_ : room;
  }
  void onCongested(uint32_t baseMs) {
    if (congested_) return;
    congested_ = true;
    ++congestionEvents_;
    if (baseMs < SNAPSHOT_MAX_GAP_MS && extra_ < SNAPSHOT_MAX_GAP_MS - baseMs) extra_ += SNAPSHOT_GAP_STEP_UP_MS;
    if (extra_ > SNAPSHOT_MAX_GAP_MS) extra_ = SNAPSHOT_MAX_GAP_MS;
  }
  void onSent() {
    ++sent_;
    if (!congested_) extra_ = extra_ > SNAPSHOT_GAP_STEP_DOWN_MS ? extra_ - SNAPSHOT_GAP_STEP_DOWN_MS : 0U;
    congested_ = false;
  }
  uint32_t sentCount() const { return sent_; }
  uint32_t congestionEvents() const { return congestionEvents_; }
  uint32_t extraMs() const { return extra_; }
 private:
  uint32_t extra_ = 0U, sent_ = 0U, congestionEvents_ = 0U;
  bool congested_ = false;
};

// Terminal acknowledgements outrank every bridge lane below them: while one waits for the wire the others stand aside - but not for
// ever, so an acknowledgement that can never be sent cannot silence the telemetry for good.
inline bool ackHolds(uint32_t waitingSince, uint32_t now) {
  return waitingSince != 0U && static_cast<uint32_t>(now - waitingSince) < ACK_PRIORITY_MAX_MS;
}

// Counters of the bridge side of the arbiter (printed with the transport's diagnostics).
struct BridgeStats {
  uint32_t ackSent = 0U, ackWireWaits = 0U, ackPublishFailed = 0U;
  uint32_t snapshotsSent = 0U, snapshotGateSkips = 0U, snapshotAckSkips = 0U, laneAckSkips = 0U;
};

}  // namespace MayapTx
