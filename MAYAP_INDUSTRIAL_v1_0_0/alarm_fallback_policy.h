#pragma once
#include <stdint.h>

// HTTPS Emergency Fallback policy for alarms (pure, host-tested).
//
// MQTT/WSS is the only normal channel. HTTPS is asleep (no socket, no TLS context, no task of its own) and wakes only for an
// alarm that MQTT demonstrably cannot carry. "Cannot carry" is classified from what the MQTT owner knows about its link, so a
// link that is about to come back is waited for, while a link that is really gone triggers the fallback at once for a critical
// alarm:
//
//   Healthy      link up and acknowledging                                       -> MQTT, never HTTPS
//   Recovering   closed on purpose / connecting, next attempt due within 6 s     -> critical waits up to 6 s, routine 60 s
//   Down         link lost, attempts failing, or the next attempt is > 6 s away  -> critical NOW, routine after 20 s
//   Flapping     3 unexpected losses within 2 min                                -> like Down (no waiting for a link that keeps dying)
//   NoAck        link up, one alarm/heartbeat PUBACK missed (adaptive wait)      -> critical NOW, routine after 20 s
//   HalfOpen     link "up" but nothing received from the broker for > 20 s       -> like NoAck
//   NotStoring   link up and alive but several PUBACKs missing: the broker or the Worker/D1 behind it is not storing events
//                                                                                -> like NoAck
//
// Back-off keeps the fallback from becoming a reconnect storm: failed HTTPS attempts walk a ladder (critical 5 s ... 5 min,
// routine 30 s ... 10 min, +<=5 s jitter) with SEPARATE timers, so a long routine back-off never delays a new critical alarm.
// A delivered attempt only imposes a 1 s gap (the alarms were real; later ones may follow at once). When MQTT delivers again the
// fallback sleeps and both ladders reset.
namespace MayapAlarmFallback {

constexpr uint32_t RECOVER_MAX_MS = 6000UL;          // "back soon": the next MQTT attempt is due within this
constexpr uint32_t CRITICAL_RECOVER_WAIT_MS = 6000UL;
constexpr uint32_t ROUTINE_RECOVER_WAIT_MS = 60000UL;
constexpr uint32_t ROUTINE_DOWN_WAIT_MS = 20000UL;
constexpr uint32_t HALF_OPEN_RX_MS = 20000UL;        // pings go out every 15 s: silence beyond this is not a healthy link
constexpr uint32_t FLAP_WINDOW_MS = 120000UL;
constexpr uint8_t FLAP_LOSSES = 3U;
constexpr uint32_t SUCCESS_GAP_MS = 1000UL;
constexpr uint32_t CRITICAL_BACKOFF_MS[] = {5000UL, 15000UL, 30000UL, 60000UL, 120000UL, 300000UL};
constexpr uint32_t ROUTINE_BACKOFF_MS[] = {30000UL, 60000UL, 120000UL, 300000UL, 600000UL};
constexpr uint8_t CRITICAL_STEPS = sizeof(CRITICAL_BACKOFF_MS) / sizeof(CRITICAL_BACKOFF_MS[0]);
constexpr uint8_t ROUTINE_STEPS = sizeof(ROUTINE_BACKOFF_MS) / sizeof(ROUTINE_BACKOFF_MS[0]);
constexpr uint32_t JITTER_MAX_MS = 5000UL;

// A device whose MQTT link carries nothing for a while would look "offline" to the Worker (it judges by last_seen, and the heartbeat
// is MQTT-only), which raises a critical DEVICE_OFFLINE although the machine is fine and only the broker path is broken. So after
// BEACON_SILENT_MS without a single durable MQTT uplink the Cloud task sends ONE small HTTPS beacon every BEACON_INTERVAL_MS (+jitter),
// through the same exclusive TLS admission, and only while MQTT cannot carry it. It stops the moment MQTT delivers again.
// The beacon is evaluated on the 60 s heartbeat tick, so with 90 s / 90 s the real cadence is 120 s: well inside the Worker's 180 s
// offline threshold (a 120 s setting would drift to 180 s and race it).
constexpr uint32_t BEACON_SILENT_MS = 90000UL;
constexpr uint32_t BEACON_INTERVAL_MS = 90000UL;

enum class Route : uint8_t { Mqtt, Wait, Https };
enum class Cause : uint8_t { Healthy, Recovering, Down, Flapping, NoAck, HalfOpen, NotStoring };

inline const char *causeText(Cause c) {
  switch (c) {
    case Cause::Healthy: return "healthy"; case Cause::Recovering: return "recovering"; case Cause::Down: return "down";
    case Cause::Flapping: return "flapping"; case Cause::NoAck: return "no-ack"; case Cause::HalfOpen: return "half-open";
    case Cause::NotStoring: return "broker-not-storing";
  }
  return "?";
}

// A copy of what the MQTT owner published (mqtt_uplink.h Health) plus the Cloud task's own counters.
struct LinkView {
  bool wifiUp = false, registered = false;
  enum class Kind : uint8_t { Closed, Connecting, Up, Down } kind = Kind::Closed;
  uint32_t nextAttemptInMs = 0xFFFFFFFFUL;   // when the transport retries (0xFFFFFFFF = unknown)
  uint8_t failures = 0U;                     // consecutive failed attempts / losses since the last Up
  uint8_t losses2min = 0U;                   // unexpected losses in the last FLAP_WINDOW_MS
  bool uplinkSuspect = false;                // an uplink event went unacknowledged recently
  uint8_t uplinkMisses = 0U;                 // consecutive unacknowledged uplink events
  uint32_t rxAgeMs = 0U;                     // time since the last byte from the broker
  bool fallbackSessionActive = false;        // WE closed MQTT for an HTTPS session
};

inline Cause classify(const LinkView &v) {
  if (v.kind == LinkView::Kind::Up) {
    if (!v.uplinkSuspect) return Cause::Healthy;
    if (v.rxAgeMs > HALF_OPEN_RX_MS) return Cause::HalfOpen;
    return v.uplinkMisses >= 2U ? Cause::NotStoring : Cause::NoAck;
  }
  if (v.losses2min >= FLAP_LOSSES) return Cause::Flapping;
  if (v.kind == LinkView::Kind::Down || v.failures > 0U || v.fallbackSessionActive) return Cause::Down;
  return v.nextAttemptInMs <= RECOVER_MAX_MS ? Cause::Recovering : Cause::Down;
}

inline uint32_t graceMs(Cause cause, bool anyCritical) {
  if (cause == Cause::Recovering) return anyCritical ? CRITICAL_RECOVER_WAIT_MS : ROUTINE_RECOVER_WAIT_MS;
  return anyCritical ? 0UL : ROUTINE_DOWN_WAIT_MS;
}

struct Gate {
  uint8_t criticalStep = 0U, routineStep = 0U;
  uint32_t nextCriticalAt = 0U, nextRoutineAt = 0U;
  bool armed = false;      // an HTTPS attempt happened since MQTT last delivered
  uint32_t attempts = 0U, failures = 0U;
  Cause episode = Cause::Healthy;   // why this fallback episode began (our own MQTT close later reads as "down" and must not hide it)
  bool ready(uint32_t now, bool critical) const {
    if (!armed) return true;
    return static_cast<int32_t>(now - (critical ? nextCriticalAt : nextRoutineAt)) >= 0;
  }
  // `jitterMs` is supplied by the caller (esp_random() on the device, fixed in tests).
  void onAttempt(uint32_t now, bool delivered, bool critical, uint32_t jitterMs) {
    ++attempts;
    armed = true;
    const uint32_t jitter = jitterMs % (JITTER_MAX_MS + 1U);
    if (delivered) {
      criticalStep = routineStep = 0U;
      nextCriticalAt = nextRoutineAt = now + SUCCESS_GAP_MS;
      return;      // `episode` is kept until MQTT delivers: the next alarms of the same outage have the same root cause
    }
    ++failures;
    if (critical) {
      nextCriticalAt = now + CRITICAL_BACKOFF_MS[criticalStep] + jitter;
      if (criticalStep + 1U < CRITICAL_STEPS) ++criticalStep;
    } else {
      nextRoutineAt = now + ROUTINE_BACKOFF_MS[routineStep] + jitter;
      if (routineStep + 1U < ROUTINE_STEPS) ++routineStep;
    }
  }
  void onMqttDelivered() { criticalStep = routineStep = 0U; armed = false; episode = Cause::Healthy; }   // fallback sleeps again
};

inline Route decide(Gate &gate, uint32_t now, const LinkView &view, uint32_t oldestPendingAgeMs, bool anyCritical,
                    Cause *causeOut = nullptr) {
  const Cause cause = classify(view);
  if (cause == Cause::Healthy) { if (causeOut) *causeOut = cause; return Route::Mqtt; }
  if (gate.episode == Cause::Healthy) gate.episode = cause;   // the first non-healthy reading names the episode
  if (causeOut) *causeOut = gate.episode;
  if (!view.wifiUp || !view.registered) return Route::Wait;
  if (oldestPendingAgeMs < graceMs(cause, anyCritical)) return Route::Wait;
  return gate.ready(now, anyCritical) ? Route::Https : Route::Wait;
}

// Is an HTTPS beacon due? `silentMs` = time since the last durable MQTT uplink (alarm or heartbeat acknowledged).
inline bool beaconDue(uint32_t now, uint32_t silentMs, uint32_t nextBeaconAt, const LinkView &view) {
  if (!view.wifiUp || !view.registered || silentMs < BEACON_SILENT_MS) return false;
  if (classify(view) == Cause::Recovering) return false;          // the link is coming back on its own
  return static_cast<int32_t>(now - nextBeaconAt) >= 0;
}

}  // namespace MayapAlarmFallback
