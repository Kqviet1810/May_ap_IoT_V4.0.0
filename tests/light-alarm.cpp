// "Light on during a batch" policy: toggling is silent; only >30 min continuously on alarms.
#include <cassert>
#include <cstdio>
#include "../MAYAP_INDUSTRIAL_v1_0_0/light_alarm_policy.h"
using namespace MayapLightAlarm;
constexpr uint32_t DELAY = 1800000UL, MIN = 60000UL;
static Action tick(Tracker &t, bool enabled, bool cond, uint32_t now) {
  const Action a = step(t, enabled, cond, now, DELAY, DELAY);
  commit(t, a, now);
  return a;
}
int main() {
  // Rapid toggling for ten minutes: nothing is ever sent, not even a "resolved".
  { Tracker t; uint32_t now = 1000;
    for (int i = 0; i < 2000; ++i, now += 300) assert(tick(t, true, (i & 1) == 0, now) == Action::None);
    assert(!t.raised); }
  // On for 29 min 59 s then off: still nothing.
  { Tracker t; uint32_t now = 5000; assert(tick(t, true, true, now) == Action::None);
    assert(tick(t, true, true, now + DELAY - 1000U) == Action::None);
    assert(tick(t, true, false, now + DELAY - 500U) == Action::None && !t.raised); }
  // Continuously on for 30 min: one alarm, silence, reminder after another 30 min, then resolved once.
  { Tracker t; uint32_t now = 100000;
    assert(tick(t, true, true, now) == Action::None);
    uint32_t raisedAt = 0; unsigned raises = 0, reminds = 0;
    for (uint32_t d = 0; d <= DELAY + 2 * MIN; d += 1000U)
      if (tick(t, true, true, now + d) == Action::Raise) { ++raises; raisedAt = now + d; }
    assert(raises == 1 && raisedAt - now >= DELAY && raisedAt - now < DELAY + 2000U);
    for (uint32_t d = 0; d < DELAY - 2000U; d += 1000U) assert(tick(t, true, true, raisedAt + 1000U + d) != Action::Raise);
    Action a = Action::None;
    for (uint32_t d = DELAY - 2000U; d < DELAY + 2000U && a != Action::Remind; d += 1000U) a = tick(t, true, true, raisedAt + 1000U + d);
    assert(a == Action::Remind); ++reminds; assert(reminds == 1);
    assert(tick(t, true, false, raisedAt + 3 * DELAY) == Action::Resolve);
    assert(tick(t, true, false, raisedAt + 3 * DELAY + 1000U) == Action::None);
    // A new episode starts the 30 min timer from scratch.
    assert(tick(t, true, true, raisedAt + 3 * DELAY + 2000U) == Action::None); }
  // A rejected enqueue (no commit) is retried on the next pass; millis wrap-around is handled.
  { Tracker t; uint32_t now = 0xFFFFFFFFUL - 1000UL;
    assert(step(t, true, true, now, DELAY, DELAY) == Action::None);
    now += DELAY + 10U;                                    // wraps past zero
    assert(step(t, true, true, now, DELAY, DELAY) == Action::Raise);   // caller could not enqueue: no commit
    assert(step(t, true, true, now + 1000U, DELAY, DELAY) == Action::Raise); }
  // Disabling the alarm drops any episode without sending anything.
  { Tracker t; uint32_t now = 1; tick(t, true, true, now); tick(t, true, true, now + DELAY);
    assert(t.raised); assert(tick(t, false, true, now + DELAY + 1U) == Action::None && !t.raised && !t.onActive); }
  printf("light alarm host tests PASS\n");
  return 0;
}
