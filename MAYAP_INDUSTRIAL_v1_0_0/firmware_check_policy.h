#pragma once
#include <stdint.h>

namespace MayapFirmwareCheck {

// When the next "is there a newer firmware?" HTTPS check is due. Pure, so it is host-tested.
//   lastCheckAt == 0 : no check yet this boot -> the first one waits `firstDelayMs` after power-up
//   otherwise        : `intervalMs` + a per-boot jitter after the previous one
// `checkNow` (operator pressed "check now") always wins. millis() wrap-around is handled.
//
// The check is an HTTPS request, and the TLS working set (~40 KiB) cannot coexist with the realtime WSS socket: while it runs the
// Web link is closed (measured: 13.4 s, ten minutes after EVERY power-up and once a day). It is a background chore, so while someone
// is using the Web (`busy` probe true) it waits, but never longer than MAX_DEFER_MS past its due time, so a tab left open for days
// cannot block updates for ever. "Check now" is never deferred.
constexpr uint32_t MAX_DEFER_MS = 6UL * 60UL * 60UL * 1000UL;
typedef bool (*BusyProbe)();
inline BusyProbe &busyProbe() { static BusyProbe probe = nullptr; return probe; }

inline bool dueByClock(uint32_t now, uint32_t lastCheckAt, uint32_t firstDelayMs, uint32_t intervalMs, uint32_t jitterMs,
                       uint32_t *lateByMs) {
  const uint32_t dueAt = lastCheckAt == 0U ? firstDelayMs : lastCheckAt + intervalMs + jitterMs;
  const int32_t late = static_cast<int32_t>(now - dueAt);
  if (lateByMs) *lateByMs = late > 0 ? static_cast<uint32_t>(late) : 0U;
  return late >= 0;
}

inline bool due(uint32_t now, uint32_t lastCheckAt, bool checkNow, uint32_t firstDelayMs,
                uint32_t intervalMs, uint32_t jitterMs) {
  if (checkNow) return true;
  uint32_t lateByMs = 0U;
  if (!dueByClock(now, lastCheckAt, firstDelayMs, intervalMs, jitterMs, &lateByMs)) return false;
  const BusyProbe busy = busyProbe();
  if (busy && lateByMs < MAX_DEFER_MS && busy()) return false;   // the Web is in use: try again on the next pass
  return true;
}

// Automatic update retries. A download that fails because of the LINK (cannot connect, HTTP error, connection lost) is tried again after
// 5, 15, then 30 minutes, at most AUTO_MAX_TRIES times for one version per boot; a failure of the release itself (size, checksum,
// signature, flash write) is tried once only, because trying again cannot help. A different version starts afresh.
constexpr uint8_t AUTO_MAX_TRIES = 5;
inline uint32_t autoRetryDelayMs(uint8_t failedTries) {
  return failedTries <= 1U ? 5UL * 60000UL : failedTries == 2U ? 15UL * 60000UL : 30UL * 60000UL;
}
inline bool autoMayTry(bool sameVersionTriedBefore, bool lastFailureRetryable, uint8_t tries, uint32_t now, uint32_t retryAt) {
  if (!sameVersionTriedBefore) return true;
  if (!lastFailureRetryable || tries >= AUTO_MAX_TRIES) return false;
  return static_cast<int32_t>(now - retryAt) >= 0;
}

}  // namespace MayapFirmwareCheck
