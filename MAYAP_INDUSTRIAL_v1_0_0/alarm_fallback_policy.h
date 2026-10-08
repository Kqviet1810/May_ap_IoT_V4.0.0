#pragma once
#include <stdint.h>

// HTTPS Emergency Fallback policy for alarms (pure, host-tested).
//
// MQTT/WSS is the only normal channel. HTTPS is asleep (no socket, no TLS context, no task of its own) and
// wakes only for an alarm that MQTT demonstrably cannot carry:
//   * the MQTT link is down / not acknowledging (`mqttDelivering == false`), AND
//   * the oldest pending alarm has waited longer than a grace period (so an ordinary reconnect or a
//     3-second realtime yield never wakes it), AND
//   * Wi-Fi is up (without it nothing can be sent anyway), AND
//   * the fallback back-off allows another attempt (no reconnect storm: 30 s, 1, 2, 5, 10 min + jitter).
// Whenever MQTT delivers again the fallback goes back to sleep and its back-off resets.
namespace MayapAlarmFallback {

constexpr uint32_t CRITICAL_GRACE_MS = 10000UL;    // safety alarms: a few reconnect attempts, then HTTPS
constexpr uint32_t ROUTINE_GRACE_MS = 60000UL;     // everything else can wait for the realtime link
constexpr uint32_t BACKOFF_MS[] = {30000UL, 60000UL, 120000UL, 300000UL, 600000UL};
constexpr uint8_t BACKOFF_STEPS = sizeof(BACKOFF_MS) / sizeof(BACKOFF_MS[0]);
constexpr uint32_t JITTER_MAX_MS = 5000UL;

enum class Route : uint8_t { Mqtt, Wait, Https };

struct Gate {
  uint8_t step = 0U;
  uint32_t nextAttemptAt = 0U;
  bool armed = false;      // an HTTPS attempt happened since MQTT last delivered
  uint32_t attempts = 0U, failures = 0U;
  bool ready(uint32_t now) const { return !armed || static_cast<int32_t>(now - nextAttemptAt) >= 0; }
  // `jitterMs` is supplied by the caller (esp_random() on the device, fixed in tests).
  void onAttempt(uint32_t now, bool delivered, uint32_t jitterMs) {
    ++attempts;
    armed = true;
    // After a success the next fallback (more alarms later) still waits the first back-off, so a flapping
    // MQTT link cannot turn HTTPS into the primary channel. Failures walk up the ladder.
    const uint8_t used = delivered ? 0U : step;
    if (delivered) step = 0U;
    else { ++failures; if (step + 1U < BACKOFF_STEPS) ++step; }
    nextAttemptAt = now + BACKOFF_MS[used] + (jitterMs % (JITTER_MAX_MS + 1U));
  }
  void onMqttDelivered() { step = 0U; armed = false; }   // fallback sleeps again
};

inline uint32_t graceMs(bool anyCritical) { return anyCritical ? CRITICAL_GRACE_MS : ROUTINE_GRACE_MS; }

inline Route decide(const Gate &gate, uint32_t now, bool mqttDelivering, bool wifiUp, bool registered,
                    uint32_t oldestPendingAgeMs, bool anyCritical) {
  if (mqttDelivering) return Route::Mqtt;
  if (!wifiUp || !registered) return Route::Wait;
  if (oldestPendingAgeMs < graceMs(anyCritical)) return Route::Wait;
  return gate.ready(now) ? Route::Https : Route::Wait;
}

}  // namespace MayapAlarmFallback
