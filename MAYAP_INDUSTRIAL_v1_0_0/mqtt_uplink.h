#pragma once
#include <Arduino.h>

// Device -> cloud uplink over the MQTT/WSS link that is already open: alarms and heartbeats are published
// on mayap/v1/<id>/alarm and /heartbeat (QoS1). The broker writes the event into its own durable queue and
// PUBACKs: a PUBACK means BROKER_STORED (not D1_STORED, not PUSH_*). Handing the event to the Worker is the broker's
// job, retried on its side, so the device never waits for D1 and a missing PUBACK says NOTHING about the transport.
//
// Transport health is judged separately, from evidence that does not depend on any acknowledgement: socket state,
// bytes received from the broker (PINGRESP included) and an explicit probe (PINGREQ sent on demand, answered or not).
// A late PUBACK only asks for such a probe; it never marks the link suspect and never makes HTTPS take the TLS slot.
//
// Two tasks meet here and neither blocks the other: the Cloud task (core 0, prio 1) OFFERS an event and
// waits a few seconds for its fate; the MQTT task (the only owner of the socket) publishes queued slots
// and reports the PUBACK. Everything is a fixed array behind one spinlock; no heap, no extra task.
//
//   Cloud:  offer() -> Queued ... Acked | Failed ... finish() -> Free        (Queued/Sent on timeout: cancel/orphan)
//   MQTT :  nextQueued() -> markSent(packetId) -> onPuback() -> Acked;  onLinkDown() -> Failed
namespace MayapUplink {

constexpr uint8_t SLOTS = 4U;
constexpr size_t PAYLOAD_MAX = 448U;            // one event (or heartbeat) as JSON
constexpr uint32_t ACK_WAIT_MAX_MS = 4000UL;    // adaptive PUBACK wait: never longer than this ...
constexpr uint32_t ACK_WAIT_MIN_MS = 1500UL;    // ... and never shorter than this (see ackWaitMs())
constexpr uint32_t SLOT_MAX_AGE_MS = 30000UL;   // safety net: no slot outlives this
constexpr uint32_t PROBE_MIN_GAP_MS = 2000UL;   // an unanswered probe is not repeated sooner than this

enum class Kind : uint8_t { Alarm, Heartbeat };
enum class State : uint8_t { Free, Queued, Sent, Acked, Failed, Orphan };

// What the MQTT owner knows about its own link, published for the Cloud task so alarm routing can tell a link that is
// about to come back from one that is really gone. Written by the MQTT task only; read as one snapshot under the lock.
enum class Link : uint8_t {
  Closed,       // deliberately closed (Cloud/OTA asked for the heap, memory pressure, portal): back after a short hold-off
  Connecting,   // not connected yet, the transport has a retry scheduled (`nextAttemptAt`)
  Up,           // connected and acknowledged by the broker
  Down,         // the link was lost / every attempt failed: recovery is not imminent
};
struct Health {
  Link link = Link::Closed;
  uint32_t since = 0U;            // when `link` last changed
  uint32_t nextAttemptAt = 0U;    // next scheduled connect attempt (0 = none scheduled)
  uint8_t failures = 0U;          // consecutive failed attempts / losses since the last Up
  uint32_t lossAt[3] = {0U, 0U, 0U};   // the last three unexpected losses (ring), 0 = none
  uint8_t lossHead = 0U;
  uint32_t lastRxAt = 0U;         // last byte received from the broker (any packet: PINGRESP, PUBACK, PUBLISH ...)
  uint16_t rttEwmaMs = 0U;        // smoothed PUBLISH->PUBACK time of uplink packets (0 = no sample yet)
  uint32_t probeSentAt = 0U;      // when the last on-demand PINGREQ left (0 = none since the link came up)
  bool probeWanted = false;       // the Cloud task asked the MQTT owner for a probe
  uint32_t probes = 0U, probesAnswered = 0U;   // diagnostics: how often a late PUBACK was cross-checked, and how often the broker answered
};

struct Slot {
  State state = State::Free;
  Kind kind = Kind::Alarm;
  uint16_t length = 0U;
  uint16_t packetId = 0U;
  uint32_t order = 0U;
  uint32_t since = 0U;
  char payload[PAYLOAD_MAX];
};

namespace Internal {
static Slot slots[SLOTS];
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static bool linkIsUp = false;
static Health health;
static uint32_t sequence = 0U;
static uint8_t yieldWhy = 0U;
}  // namespace Internal

// Why the Cloud task is about to take the TLS slot from MQTT, so every deliberate MQTT close carries its evidence in the log.
// Only the alarm fallback (link confirmed gone / half-open / starved critical) and the broker-outage beacon are alarm reasons;
// everything else (registration, firmware check, OTA) is `Other`. A late PUBACK is not a reason.
enum class YieldWhy : uint8_t { Other, AlarmDown, AlarmFlapping, AlarmHalfOpen, AlarmStarved, Beacon };
inline void setYieldWhy(YieldWhy why) { __atomic_store_n(&Internal::yieldWhy, static_cast<uint8_t>(why), __ATOMIC_RELEASE); }
inline YieldWhy yieldWhy() { return static_cast<YieldWhy>(__atomic_load_n(&Internal::yieldWhy, __ATOMIC_ACQUIRE)); }
inline const char *yieldWhyText(YieldWhy why) {
  switch (why) {
    case YieldWhy::AlarmDown: return "alarm:link-down"; case YieldWhy::AlarmFlapping: return "alarm:link-flapping";
    case YieldWhy::AlarmHalfOpen: return "alarm:half-open-confirmed"; case YieldWhy::AlarmStarved: return "alarm:critical-starved";
    case YieldWhy::Beacon: return "beacon:broker-unreachable"; default: return "other";
  }
}

// ---------------------------- Cloud task side ----------------------------

// True while the MQTT owner has the link open. A missing PUBACK never changes this: only the owner decides the link is gone.
inline bool available(uint32_t) {
  portENTER_CRITICAL(&Internal::mux);
  const bool ok = Internal::linkIsUp;
  portEXIT_CRITICAL(&Internal::mux);
  return ok;
}

// Cloud task: "an acknowledgement is late - cross-check the transport". Cheap and idempotent; the owner sends one PINGREQ.
inline void requestProbe(uint32_t now) {
  using namespace Internal;
  portENTER_CRITICAL(&mux);
  const bool unanswered = health.probeSentAt != 0U && static_cast<int32_t>(health.lastRxAt - health.probeSentAt) <= 0;
  const bool tooSoon = health.probeSentAt != 0U && static_cast<uint32_t>(now - health.probeSentAt) < PROBE_MIN_GAP_MS;
  if (!unanswered && !tooSoon) health.probeWanted = true;
  portEXIT_CRITICAL(&mux);
}

// Returns the slot index, or -1 when every slot is busy or the payload does not fit.
inline int8_t offer(Kind kind, const char *json, size_t length, uint32_t now) {
  using namespace Internal;
  if (!json || length == 0U || length >= PAYLOAD_MAX) return -1;
  int8_t taken = -1;
  portENTER_CRITICAL(&mux);
  if (linkIsUp) {
    for (uint8_t i = 0U; i < SLOTS; ++i) {
      if (slots[i].state != State::Free) continue;
      memcpy(slots[i].payload, json, length);
      slots[i].payload[length] = '\0';
      slots[i].length = static_cast<uint16_t>(length);
      slots[i].kind = kind;
      slots[i].packetId = 0U;
      slots[i].order = ++sequence;
      slots[i].since = now;
      slots[i].state = State::Queued;
      taken = static_cast<int8_t>(i);
      break;
    }
  }
  portEXIT_CRITICAL(&mux);
  return taken;
}

inline State peek(int8_t index) {
  using namespace Internal;
  if (index < 0 || index >= static_cast<int8_t>(SLOTS)) return State::Free;
  portENTER_CRITICAL(&mux);
  const State s = slots[index].state;
  portEXIT_CRITICAL(&mux);
  return s;
}

inline bool linkUp() {
  portENTER_CRITICAL(&Internal::mux);
  const bool up = Internal::linkIsUp;
  portEXIT_CRITICAL(&Internal::mux);
  return up;
}

// Ends the Cloud task's interest in a slot and returns what it was: Acked (the event is durable), Failed,
// or - when the wait ran out - Queued (cancelled before it was sent) / Sent (published, outcome unknown:
// the slot becomes an orphan the MQTT task frees on PUBACK or link loss; the event id makes a resend safe).
inline State finish(int8_t index) {
  using namespace Internal;
  if (index < 0 || index >= static_cast<int8_t>(SLOTS)) return State::Free;
  portENTER_CRITICAL(&mux);
  const State s = slots[index].state;
  if (s == State::Acked || s == State::Failed || s == State::Queued) slots[index].state = State::Free;
  else if (s == State::Sent) slots[index].state = State::Orphan;
  portEXIT_CRITICAL(&mux);
  return s;
}

// ------------------------------ MQTT task side ------------------------------

inline void onLinkUp() {
  portENTER_CRITICAL(&Internal::mux);
  Internal::linkIsUp = true;
  portEXIT_CRITICAL(&Internal::mux);
}

// The socket is gone: nothing queued or in flight can complete any more.
inline void onLinkDown() {
  using namespace Internal;
  portENTER_CRITICAL(&mux);
  linkIsUp = false;
  for (uint8_t i = 0U; i < SLOTS; ++i) {
    if (slots[i].state == State::Queued || slots[i].state == State::Sent) slots[i].state = State::Failed;
    else if (slots[i].state == State::Orphan) slots[i].state = State::Free;
  }
  portEXIT_CRITICAL(&mux);
}

// Oldest queued slot (FIFO: an alarm and its recovery keep their order), or -1.
inline int8_t nextQueued() {
  using namespace Internal;
  int8_t best = -1;
  portENTER_CRITICAL(&mux);
  for (uint8_t i = 0U; i < SLOTS; ++i) {
    if (slots[i].state != State::Queued) continue;
    if (best < 0 || static_cast<int32_t>(slots[i].order - slots[best].order) < 0) best = static_cast<int8_t>(i);
  }
  portEXIT_CRITICAL(&mux);
  return best;
}

// The payload of a Queued/Sent slot is immutable until its state leaves those two, so the MQTT task reads
// it without the lock.
inline const char *payload(int8_t index) { return Internal::slots[index].payload; }
inline size_t length(int8_t index) { return Internal::slots[index].length; }
inline Kind kind(int8_t index) { return Internal::slots[index].kind; }

inline void markSent(int8_t index, uint16_t packetId, uint32_t now) {
  using namespace Internal;
  portENTER_CRITICAL(&mux);
  if (slots[index].state == State::Queued) {
    slots[index].state = State::Sent;
    slots[index].packetId = packetId;
    slots[index].since = now;
  }
  portEXIT_CRITICAL(&mux);
}

inline void markFailed(int8_t index) {
  using namespace Internal;
  portENTER_CRITICAL(&mux);
  if (slots[index].state == State::Queued || slots[index].state == State::Sent) slots[index].state = State::Failed;
  portEXIT_CRITICAL(&mux);
}

inline void onPuback(uint16_t packetId, uint32_t now = 0U) {
  using namespace Internal;
  portENTER_CRITICAL(&mux);
  for (uint8_t i = 0U; i < SLOTS; ++i) {
    if (slots[i].packetId != packetId) continue;
    if (slots[i].state == State::Sent && now != 0U) {
      // Smoothed round trip PUBLISH -> PUBACK (1/4 weight), clamped to a sane range: the broker only acknowledges
      // after the Worker stored the event, so this includes the ingest time and tracks how healthy the path is.
      uint32_t rtt = static_cast<uint32_t>(now - slots[i].since);
      if (rtt < 20U) rtt = 20U;
      if (rtt > 10000U) rtt = 10000U;
      const uint32_t ewma = health.rttEwmaMs;
      health.rttEwmaMs = static_cast<uint16_t>(ewma == 0U ? rtt : ewma - ewma / 4U + rtt / 4U);
    }
    if (slots[i].state == State::Sent) slots[i].state = State::Acked;
    else if (slots[i].state == State::Orphan) slots[i].state = State::Free;
  }
  portEXIT_CRITICAL(&mux);
}

// Called every pass by the MQTT task: no slot may outlive SLOT_MAX_AGE_MS whatever the Cloud task does.
inline void expire(uint32_t now) {
  using namespace Internal;
  portENTER_CRITICAL(&mux);
  for (uint8_t i = 0U; i < SLOTS; ++i) {
    if (slots[i].state == State::Free || static_cast<uint32_t>(now - slots[i].since) < SLOT_MAX_AGE_MS) continue;
    const State s = slots[i].state;   // Queued/Sent never completed; nobody collected an Acked/Failed/Orphan slot
    slots[i].state = (s == State::Queued || s == State::Sent) ? State::Failed : State::Free;
  }
  portEXIT_CRITICAL(&mux);
}

// ------------------------------ link health ------------------------------

// How long the Cloud task waits for an uplink PUBACK before it declares the event unacknowledged: three smoothed round
// trips plus half a second, bounded to [ACK_WAIT_MIN_MS, ACK_WAIT_MAX_MS]. No sample yet -> the upper bound.
inline uint32_t ackWaitMs() {
  portENTER_CRITICAL(&Internal::mux);
  const uint32_t ewma = Internal::health.rttEwmaMs;
  portEXIT_CRITICAL(&Internal::mux);
  if (ewma == 0U) return ACK_WAIT_MAX_MS;
  uint32_t wait = 3U * ewma + 500U;
  if (wait < ACK_WAIT_MIN_MS) wait = ACK_WAIT_MIN_MS;
  if (wait > ACK_WAIT_MAX_MS) wait = ACK_WAIT_MAX_MS;
  return wait;
}

// MQTT-task side: record transitions. `nextAttemptAt` is the transport's own schedule (0 = none).
inline void healthUp(uint32_t now) {
  portENTER_CRITICAL(&Internal::mux);
  Health &h = Internal::health;
  h.link = Link::Up; h.since = now; h.nextAttemptAt = 0U; h.failures = 0U; h.lastRxAt = now;
  h.probeSentAt = 0U; h.probeWanted = false;
  portEXIT_CRITICAL(&Internal::mux);
}
inline void healthClosed(uint32_t now, uint32_t nextAttemptAt) {          // deliberate: not a failure
  portENTER_CRITICAL(&Internal::mux);
  Health &h = Internal::health;
  if (h.link != Link::Closed) { h.link = Link::Closed; h.since = now; }
  h.nextAttemptAt = nextAttemptAt;
  portEXIT_CRITICAL(&Internal::mux);
}
inline void healthLost(uint32_t now, uint32_t nextAttemptAt) {            // an established link died
  portENTER_CRITICAL(&Internal::mux);
  Health &h = Internal::health;
  h.link = Link::Down; h.since = now; h.nextAttemptAt = nextAttemptAt;
  if (h.failures < 255U) ++h.failures;
  h.lossAt[h.lossHead] = now ? now : 1U;
  h.lossHead = static_cast<uint8_t>((h.lossHead + 1U) % 3U);
  portEXIT_CRITICAL(&Internal::mux);
}
inline void healthAttemptFailed(uint32_t now, uint32_t nextAttemptAt) {   // a connect attempt failed
  portENTER_CRITICAL(&Internal::mux);
  Health &h = Internal::health;
  if (h.link != Link::Down) { h.link = Link::Down; h.since = now; }
  h.nextAttemptAt = nextAttemptAt;
  if (h.failures < 255U) ++h.failures;
  portEXIT_CRITICAL(&Internal::mux);
}
inline void healthWaiting(uint32_t now, uint32_t nextAttemptAt) {         // idle, an attempt is scheduled
  portENTER_CRITICAL(&Internal::mux);
  Health &h = Internal::health;
  if (h.link == Link::Closed || h.link == Link::Up) { h.link = Link::Connecting; h.since = now; }
  h.nextAttemptAt = nextAttemptAt;
  portEXIT_CRITICAL(&Internal::mux);
}
inline void healthRx(uint32_t at) {
  portENTER_CRITICAL(&Internal::mux);
  Health &h = Internal::health;
  if (h.probeSentAt != 0U && static_cast<int32_t>(h.lastRxAt - h.probeSentAt) <= 0 && static_cast<int32_t>(at - h.probeSentAt) > 0) ++h.probesAnswered;
  h.lastRxAt = at;
  portEXIT_CRITICAL(&Internal::mux);
}
// MQTT owner: is a probe requested? (clears the request). noteProbeSent() stamps the PINGREQ that went out for it.
inline bool takeProbeRequest() {
  portENTER_CRITICAL(&Internal::mux);
  const bool wanted = Internal::health.probeWanted;
  Internal::health.probeWanted = false;
  portEXIT_CRITICAL(&Internal::mux);
  return wanted;
}
inline void noteProbeSent(uint32_t now) {
  portENTER_CRITICAL(&Internal::mux);
  Internal::health.probeSentAt = now ? now : 1U;
  ++Internal::health.probes;
  portEXIT_CRITICAL(&Internal::mux);
}
// Probe state as the Cloud task needs it: pending = a probe left and nothing at all came back since.
inline bool probePending(const Health &h) {
  return h.probeSentAt != 0U && static_cast<int32_t>(h.lastRxAt - h.probeSentAt) <= 0;
}
inline Health healthSnapshot() {
  portENTER_CRITICAL(&Internal::mux);
  const Health copy = Internal::health;
  portEXIT_CRITICAL(&Internal::mux);
  return copy;
}
// Unexpected losses inside the last `windowMs` (the ring keeps three, so 3 means "three or more").
inline uint8_t recentLosses(const Health &h, uint32_t now, uint32_t windowMs) {
  uint8_t count = 0U;
  for (uint8_t i = 0U; i < 3U; ++i)
    if (h.lossAt[i] != 0U && static_cast<uint32_t>(now - h.lossAt[i]) <= windowMs) ++count;
  return count;
}

}  // namespace MayapUplink
