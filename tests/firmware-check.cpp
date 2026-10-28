// Web firmware check cadence (firmware_check_policy.h): first check after power-up delay, then daily + jitter.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include "../MAYAP_INDUSTRIAL_v1_0_0/firmware_check_policy.h"
using MayapFirmwareCheck::due;
constexpr uint32_t MIN = 60000UL, FIRST = 10 * MIN, DAY = 24UL * 60UL * MIN, JIT = 30 * MIN;
int main() {
  // Boot: nothing before the first-check delay (MQTT/Cloud settle), then due.
  assert(!due(1000, 0, false, FIRST, DAY, 0) && !due(FIRST - 1, 0, false, FIRST, DAY, 0));
  assert(due(FIRST, 0, false, FIRST, DAY, 0) && due(FIRST + 5, 0, false, FIRST, DAY, 0));
  // "Check now" is never delayed, at boot or mid-interval.
  assert(due(1, 0, true, FIRST, DAY, JIT) && due(100000, 50000, true, FIRST, DAY, JIT));
  // After a check: exactly interval + this boot's jitter.
  for (uint32_t jitter : {uint32_t(0), uint32_t(1), uint32_t(JIT / 2), uint32_t(JIT)}) {
    const uint32_t last = 12 * MIN;
    assert(!due(last + DAY + jitter - 1, last, false, FIRST, DAY, jitter));
    assert(due(last + DAY + jitter, last, false, FIRST, DAY, jitter));
  }
  // A busy TLS lease does not move lastCheckAt, so the check stays due (retried by the caller next loop).
  assert(due(FIRST + 3 * MIN, 0, false, FIRST, DAY, JIT));
  // millis() wrap-around: 49.7 days of uptime.
  const uint32_t last = 0xFFFFFFFFUL - 5 * MIN;
  assert(!due(last + DAY - 1, last, false, FIRST, DAY, 0) && due(last + DAY, last, false, FIRST, DAY, 0));
  // While the Web is in use the (HTTPS) check waits, because it closes the realtime link; "check now" never waits; and a tab left open
  // for days cannot postpone it for ever.
  static bool webInUse = true;
  MayapFirmwareCheck::busyProbe() = [] { return webInUse; };
  assert(!due(FIRST + 5, 0, false, FIRST, DAY, 0));                                  // due by the clock, deferred by the Web
  assert(due(FIRST + 5, 0, true, FIRST, DAY, 0));                                    // operator pressed "check now"
  assert(!due(FIRST + MayapFirmwareCheck::MAX_DEFER_MS - 1, 0, false, FIRST, DAY, 0));
  assert(due(FIRST + MayapFirmwareCheck::MAX_DEFER_MS, 0, false, FIRST, DAY, 0));      // deferred long enough: runs anyway
  webInUse = false;
  assert(due(FIRST + 5, 0, false, FIRST, DAY, 0));                                   // Web idle: runs at once
  assert(!due(FIRST - 1, 0, false, FIRST, DAY, 0));                                  // never before its time
  webInUse = true;
  const uint32_t lastOk = 12 * MIN;
  assert(!due(lastOk + DAY + 10, lastOk, false, FIRST, DAY, 0));
  assert(due(lastOk + DAY + MayapFirmwareCheck::MAX_DEFER_MS, lastOk, false, FIRST, DAY, 0));
  MayapFirmwareCheck::busyProbe() = nullptr;
  // Automatic update retries: a link failure is repeated after 5, 15, 30, 30 ... minutes (at most AUTO_MAX_TRIES), a failure of the release itself is not,
  // a new version starts afresh, and millis() wrap-around is safe.
  using MayapFirmwareCheck::autoMayTry; using MayapFirmwareCheck::autoRetryDelayMs;
  assert(autoMayTry(false, false, 0, 0, 0));                                          // first attempt for a version
  assert(autoRetryDelayMs(1) == 5 * MIN && autoRetryDelayMs(2) == 15 * MIN && autoRetryDelayMs(3) == 30 * MIN && autoRetryDelayMs(9) == 30 * MIN);
  const uint32_t failedAt = 1000000UL;
  assert(!autoMayTry(true, true, 1, failedAt + 5 * MIN - 1, failedAt + 5 * MIN));    // too early
  assert(autoMayTry(true, true, 1, failedAt + 5 * MIN, failedAt + 5 * MIN));         // time to repeat a link failure
  assert(!autoMayTry(true, false, 1, failedAt + 99 * MIN, failedAt + 5 * MIN));      // a bad release is not repeated
  assert(!autoMayTry(true, true, MayapFirmwareCheck::AUTO_MAX_TRIES, failedAt + 99 * MIN, failedAt));   // out of tries
  assert(autoMayTry(true, true, MayapFirmwareCheck::AUTO_MAX_TRIES - 1, failedAt + 99 * MIN, failedAt));
  assert(autoMayTry(false, false, MayapFirmwareCheck::AUTO_MAX_TRIES, 0, 0));        // a different version starts afresh
  const uint32_t wrapRetry = 0xFFFFFFFFUL - MIN;                                     // retry time just before the wrap
  assert(!autoMayTry(true, true, 1, wrapRetry - 1, wrapRetry) && autoMayTry(true, true, 1, 3 * MIN, wrapRetry));
  std::puts("firmware check cadence host tests PASS");
  return 0;
}
