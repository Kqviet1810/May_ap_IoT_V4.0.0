#include <cassert>
#include <cstdint>
#include <cstdio>
#include "../MAYAP_INDUSTRIAL_v1_0_0/runtime_recovery_policy.h"
using namespace MayapRecovery;
int main() {
  ServiceWatch watch;
  assert(watch.update(900000, false, 0, 0, 30000) == Action::None);
  assert(watch.update(30000, true, 0, 0, 30000) == Action::None);
  assert(watch.update(30001, true, 0, 0, 30000) == Action::Reinit);
  assert(watch.update(90000, true, 0, 0, 30000) == Action::None);
  assert(watch.update(90001, true, 0, 0, 30000) == Action::Isolate);
  assert(watch.update(330000, true, 0, 0, 30000) == Action::None);
  assert(watch.update(330001, true, 0, 0, 30000) == Action::Degraded);
  assert(watch.update(330002, true, 0, 0, 30000) == Action::None);
  assert(watch.update(330003, true, 330003, 1, 30000) == Action::None);
  assert(watch.update(360004, true, 330003, 1, 30000) == Action::Reinit);
  ServiceWatch offline;
  for (uint32_t now = 100; now < 10000000; now += 100)
    assert(offline.update(now, true, now, 0, 30000) == Action::None);
  ServiceWatch rollover;
  assert(rollover.update(0xFFFFF000U, true, 0xFFFFF000U, 0, 30000) == Action::None);
  assert(rollover.update(0xFFFFF000U + 30001U, true, 0xFFFFF000U, 0, 30000) == Action::Reinit);
  assert(rollover.update(0xFFFFF000U + 90001U, true, 0xFFFFF000U, 0, 30000) == Action::Isolate);
  assert(rollover.update(0xFFFFF000U + 330001U, true, 0xFFFFF000U, 0, 30000) == Action::Degraded);
  assert(rollover.update(0xFFFFF000U + 330002U, true, 0xFFFFF000U, 0, 30000) == Action::None);
  // A healthy service that beats on the other core between the supervisor reading `now` and `beat` is NOT stale.
  ServiceWatch race;
  assert(race.update(1000000, true, 1000005, 0, 60000) == Action::None);          // beat 5 ms newer than now
  assert(race.update(1000000, true, 1000000 + 40000, 0, 60000) == Action::None);  // even 40 s "in the future"
  assert(silence(1000000, 1000005) == 0U && silence(1000005, 1000000) == 5U);
  ServiceWatch wrap;                                                                // now just after the 32-bit wrap, beat just before
  assert(wrap.update(5U, true, 0xFFFFFFF0U, 0, 60000) == Action::None);
  assert(silence(5U, 0xFFFFFFF0U) == 21U);
  assert(wrap.update(60022U, true, 0xFFFFFFF0U, 0, 60000) == Action::Reinit);      // genuinely silent for 60.032 s
  // Wi-Fi escalation (6 failures / 5 min outage / cooldown) moved into wifi_fsm.h: tests/wifi-fsm.cpp.
  std::puts("Runtime policy: online services reinit/isolate/degrade without controller restart PASS");
}
