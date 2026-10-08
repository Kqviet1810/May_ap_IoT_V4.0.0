#pragma once
#include <stdint.h>

namespace MayapFirmwareCheck {

// When the next "is there a newer firmware?" HTTPS check is due. Pure, so it is host-tested.
//   lastCheckAt == 0 : no check yet this boot -> the first one waits `firstDelayMs` after power-up
//   otherwise        : `intervalMs` + a per-boot jitter after the previous one
// `checkNow` (operator pressed "check now") always wins. millis() wrap-around is handled.
inline bool due(uint32_t now, uint32_t lastCheckAt, bool checkNow, uint32_t firstDelayMs,
                uint32_t intervalMs, uint32_t jitterMs) {
  if (checkNow) return true;
  if (lastCheckAt == 0U) return static_cast<int32_t>(now - firstDelayMs) >= 0;
  return static_cast<uint32_t>(now - lastCheckAt) >= intervalMs + jitterMs;
}

}  // namespace MayapFirmwareCheck
