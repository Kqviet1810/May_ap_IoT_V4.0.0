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
  std::puts("firmware check cadence host tests PASS");
  return 0;
}
