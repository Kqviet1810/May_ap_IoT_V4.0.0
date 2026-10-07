#pragma once
#include <Arduino.h>

// Device -> cloud uplink over the MQTT/WSS link that is already open: alarms and heartbeats are published
// on mayap/v1/<id>/alarm and /heartbeat (QoS1). The broker hands them to the main Worker and sends the
// PUBACK only after the Worker confirmed a durable write, so a PUBACK means "stored", exactly like the
// HTTPS durable receipt - but without a second TLS session (~70 kB of heap) that forced realtime to yield.
//
// Two tasks meet here and neither blocks the other: the Cloud task (core 0, prio 1) OFFERS an event and
// waits a few seconds for its fate; the MQTT task (the only owner of the socket) publishes queued slots
// and reports the PUBACK. Everything is a fixed array behind one spinlock; no heap, no extra task.
//
//   Cloud:  offer() -> Queued ... Acked | Failed ... finish() -> Free        (Queued/Sent on timeout: cancel/orphan)
//   MQTT :  nextQueued() -> markSent(packetId) -> onPuback() -> Acked;  onLinkDown() -> Failed
namespace MayapUplink {

constexpr uint8_t SLOTS = 4U;
constexpr size_t PAYLOAD_MAX = 400U;            // one event (or heartbeat) as JSON
constexpr uint32_t ACK_WAIT_MS = 4000UL;        // how long the Cloud task waits for the PUBACK
constexpr uint32_t SLOT_MAX_AGE_MS = 30000UL;   // safety net: no slot outlives this
constexpr uint32_t SUSPECT_MS = 60000UL;        // after a miss, Cloud uses HTTPS for this long

enum class Kind : uint8_t { Alarm, Heartbeat };
enum class State : uint8_t { Free, Queued, Sent, Acked, Failed, Orphan };

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
static uint32_t suspectUntil = 0U;
static uint32_t sequence = 0U;
}  // namespace Internal

// ---------------------------- Cloud task side ----------------------------

inline bool available(uint32_t now) {
  using namespace Internal;
  portENTER_CRITICAL(&mux);
  const bool ok = linkIsUp && (suspectUntil == 0U || static_cast<int32_t>(now - suspectUntil) >= 0);
  portEXIT_CRITICAL(&mux);
  return ok;
}

inline void suspect(uint32_t now) {
  using namespace Internal;
  portENTER_CRITICAL(&mux);
  suspectUntil = now + SUSPECT_MS;
  if (suspectUntil == 0U) suspectUntil = 1U;
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

inline void onPuback(uint16_t packetId) {
  using namespace Internal;
  portENTER_CRITICAL(&mux);
  for (uint8_t i = 0U; i < SLOTS; ++i) {
    if (slots[i].packetId != packetId) continue;
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

}  // namespace MayapUplink
