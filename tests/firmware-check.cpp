// Web firmware check cadence (firmware_check_policy.h): first check after power-up delay, then daily + jitter.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include "../MAYAP_INDUSTRIAL_v1_0_0/firmware_check_policy.h"
#include "../MAYAP_INDUSTRIAL_v1_0_0/maintenance_policy.h"
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
  // ---- when the machine may be updated (maintenance_policy.h) ----
  using namespace MayapMaintenance;
  auto base = [] { ReadyInputs in; in.sensorUsable = true; in.temperature = 37.5f; in.setpoint = 37.5f; return in; };
  { ReadyInputs in = base(); assert(ready(in)); }                                              // idle machine, no batch
  { ReadyInputs in = base(); in.batchRunning = true; assert(!ready(in)); }                     // a batch without developer mode: never
  { ReadyInputs in = base(); in.batchRunning = true; in.developerMode = true; assert(ready(in)); }      // developer mode, PV on SV
  { ReadyInputs in = base(); in.batchRunning = true; in.developerMode = true; in.temperature = 38.5f; assert(ready(in)); }   // exactly +1.0
  { ReadyInputs in = base(); in.batchRunning = true; in.developerMode = true; in.temperature = 38.6f; assert(!ready(in)); }
  { ReadyInputs in = base(); in.batchRunning = true; in.developerMode = true; in.temperature = 36.4f; assert(!ready(in)); }
  { ReadyInputs in = base(); in.batchRunning = true; in.developerMode = true; in.setpoint = 30.0f; in.temperature = 30.4f; assert(ready(in)); }   // the band follows SV
  { ReadyInputs in = base(); in.batchRunning = true; in.developerMode = true; in.setpoint = 30.0f; in.temperature = 37.5f; assert(!ready(in)); }
  { ReadyInputs in = base(); in.batchRunning = true; in.developerMode = true; in.sensorUsable = false; assert(!ready(in)); }
  { ReadyInputs in = base(); in.batchRunning = true; in.developerMode = true; in.temperature = NAN; assert(!ready(in)); }
  { ReadyInputs in = base(); in.batchRunning = true; in.developerMode = true; in.highTemp = true; assert(!ready(in)); }
  for (int bit = 0; bit < 3; ++bit) {                                                          // test mode, turning, emergency block everything
    ReadyInputs in = base(); in.developerMode = true; if (bit == 0) in.testMode = true; else if (bit == 1) in.turning = true; else in.emergency = true;
    assert(!ready(in)); in.batchRunning = true; assert(!ready(in));
  }
  for (int pending = 0; pending < 2; ++pending) {                                              // waiting to resume / to record a stop: never, even in developer mode
    ReadyInputs in = base(); in.developerMode = true;
    if (pending == 0) in.resumePending = true; else in.batchClearPending = true;
    assert(!ready(in)); in.batchRunning = true; assert(!ready(in));
  }
  // ---- remote command spacing and "update now" throttle ----
  assert(commandGapOk(5000, 0, REMOTE_OUTPUT_GAP_MS, false));                                  // first command
  assert(!commandGapOk(5000 + REMOTE_OUTPUT_GAP_MS - 1, 5000, REMOTE_OUTPUT_GAP_MS, false) && commandGapOk(5000 + REMOTE_OUTPUT_GAP_MS, 5000, REMOTE_OUTPUT_GAP_MS, false));
  assert(commandGapOk(5001, 5000, REMOTE_OUTPUT_GAP_MS, true));                                // restoring a safe state is never delayed
  assert(commandGapOk(1500, 0xFFFFFFFFUL - 500, REMOTE_OUTPUT_GAP_MS, false));                // millis() wrap
  assert(applyAllowed(1000, false, 0) && !applyAllowed(1000, true, 0));                        // never while downloading
  assert(!applyAllowed(1000 + RETRY_AFTER_FAIL_MS - 1, false, 1000) && applyAllowed(1000 + RETRY_AFTER_FAIL_MS, false, 1000));
  assert(HEATER_HOLD_MS <= 30000UL);                                                           // the heater is never held for long
  std::puts("firmware check cadence host tests PASS");
  return 0;
}
