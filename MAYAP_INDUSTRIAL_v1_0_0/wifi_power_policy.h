#pragma once
#include <stdint.h>

// Wi-Fi power policy. ECO (modem sleep after a long idle) is behind MAYAP_WIFI_ECO and is OFF by default: until it has been
// soaked on hardware (alarm latency, MQTT keep-alive through DTIM, reconnects) the radio stays in PERFORMANCE (no power save)
// at all times. With ECO on: fixed PERFORMANCE while the Web is in use (and for 15 minutes after), modem sleep
// afterwards. One slow, time-based rule - NOT the old per-lease / per-error toggling that was removed for
// instability: it only ever enters modem sleep after a long quiet period and returns to PERFORMANCE immediately
// on the first Web activity or alarm, so realtime and alarm delivery are delayed by a sleeping radio at most
// until the next DTIM beacon.
#ifndef MAYAP_WIFI_ECO
#define MAYAP_WIFI_ECO 0
#endif

namespace MayapWifiPower {

constexpr bool ECO_ENABLED = MAYAP_WIFI_ECO != 0;

constexpr uint32_t WEB_IDLE_MS = 15UL * 60UL * 1000UL;   // no Web use for 15 min -> save
constexpr uint32_t ALARM_AWAKE_MS = 2UL * 60UL * 1000UL; // an alarm keeps the radio awake while it is delivered

enum class Mode : uint8_t { Performance, Save };

namespace Internal {
static volatile uint32_t webAt = 0U;          // 0 = boot: the first 15 minutes after power-up stay awake
static volatile uint32_t alarmAt = 0U;
static volatile uint8_t alarmSeen = 0U;
}  // namespace Internal

// Any Web contact: an active `session`, a signed command, a config/history request.
inline void noteWebActivity(uint32_t now) { Internal::webAt = now; }
// An alarm is queued / being delivered.
inline void noteAlarmActivity(uint32_t now) { Internal::alarmAt = now; Internal::alarmSeen = 1U; }

// Modem sleep only after BOTH quiet periods have elapsed; either kind of activity returns to PERFORMANCE at once.
// (Both periods are minutes, so the mode can never flap faster than that.)
inline Mode desired(uint32_t now) {
  if (!ECO_ENABLED) return Mode::Performance;
  const bool webIdle = static_cast<uint32_t>(now - Internal::webAt) >= WEB_IDLE_MS;
  const bool alarmQuiet = !Internal::alarmSeen || static_cast<uint32_t>(now - Internal::alarmAt) >= ALARM_AWAKE_MS;
  return webIdle && alarmQuiet ? Mode::Save : Mode::Performance;
}

}  // namespace MayapWifiPower
