// Host test of the vent hold (MAYAP_INDUSTRIAL_v1_0_0/vent_hold.h): the profile / scheduled ventilation waits while the oven climbs, recovers from a door or stands
// far under SP, at the start of a batch and at any time of it; it cannot hold for ever.
#include "../MAYAP_INDUSTRIAL_v1_0_0/vent_hold.h"
#include <cstdio>

using namespace VentHoldRule;
static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; std::printf("  FAIL %s:%d  %s  --  ", __FILE__, __LINE__, #cond); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main() {
  const Params p;
  const uint32_t MIN = 60000U;
  // 1) the first climb: held until PV is within 0.2 under SP; not held again by the hysteresis band 0.2 .. 0.3
  { Hold h; uint32_t t = 1000;
    CHECK(h.update(t, true, true, 9.0f, false, p), "first climb: hold");
    t += 20 * MIN; CHECK(h.update(t, true, true, 0.6f, false, p), "still 0.6 under: hold");
    t += MIN; CHECK(!h.update(t, true, true, 0.2f, false, p), "0.2 under: released");
    t += MIN; CHECK(!h.update(t, true, true, 0.25f, false, p), "0.25 under: inside the band, not held again");
    t += MIN; CHECK(h.update(t, true, true, 0.35f, false, p), "0.35 under: held again"); }
  // 2) a door at any time of the batch: held at once, released when PV is back and the door is gone
  { Hold h; uint32_t t = 1000;
    CHECK(!h.update(t, true, true, 0.0f, false, p), "settled batch: no hold");
    t += MIN; CHECK(h.update(t, true, true, 0.1f, true, p), "door seen with PV still near SP: hold");
    t += MIN; CHECK(h.update(t, true, true, 0.1f, true, p), "PV near SP but door still seen: hold");
    t += MIN; CHECK(h.update(t, true, true, 1.5f, false, p), "door gone, PV 1.5 under: hold");
    t += 10 * MIN; CHECK(!h.update(t, true, true, 0.15f, false, p), "recovered: released"); }
  // 3) a batch that starts before the first usable sample: held (for at most 30 min); the first sample decides
  { Hold h; uint32_t t = 1000;
    CHECK(h.update(t, true, false, NAN, false, p), "sensor not usable at the start: hold");
    t += 5 * MIN; CHECK(!h.update(t, true, true, 0.1f, false, p), "first usable sample near SP: no hold");
    Hold g; t = 1000; CHECK(g.update(t, true, false, NAN, false, p), "again: hold");
    t += 31 * MIN; CHECK(!g.update(t, true, false, NAN, false, p), "sensor never usable: 30 min bound"); }
  // 4) a batch that never reaches SP: the hold ends after 30 min and does not come back until PV has been at 0.2 once
  { Hold h; uint32_t t = 1000;
    CHECK(h.update(t, true, true, 3.0f, false, p), "far under: hold");
    t += 29 * MIN; CHECK(h.update(t, true, true, 3.0f, false, p), "29 min: still holding");
    t += 2 * MIN; CHECK(!h.update(t, true, true, 3.0f, false, p), "31 min: the bound ends it");
    t += 5 * MIN; CHECK(!h.update(t, true, true, 3.0f, false, p), "still far under: not held again (expired)");
    t += MIN; CHECK(!h.update(t, true, true, 0.1f, false, p), "back at SP: expiry cleared");
    t += MIN; CHECK(h.update(t, true, true, 1.0f, false, p), "a new dip: held again"); }
  // 5) the sensor goes bad in the middle of a hold: released (the forced vents decide then)
  { Hold h; uint32_t t = 1000; h.update(t, true, true, 2.0f, false, p);
    t += MIN; CHECK(!h.update(t, true, false, NAN, false, p), "sensor lost: released"); }
  // 6) no batch: never held; a stopped batch resets everything
  { Hold h; uint32_t t = 1000;
    CHECK(!h.update(t, false, true, 5.0f, true, p), "no batch: never");
    h.update(t, true, true, 5.0f, false, p); t += MIN; CHECK(!h.update(t, false, true, 5.0f, false, p), "batch stopped");
    t += MIN; CHECK(h.update(t, true, true, 5.0f, false, p), "a new batch starts from scratch"); }
  // 7) the millisecond clock wraps
  { Hold h; uint32_t t = 0xFFFFFFFFU - 5 * MIN;
    CHECK(h.update(t, true, true, 4.0f, false, p), "near the wrap: hold");
    t += 31 * MIN; CHECK(!h.update(t, true, true, 4.0f, false, p), "across the wrap: the 30 min bound still ends it"); }
  if (failures == 0) std::printf("VENT HOLD: ALL PASSED\n");
  return failures;
}
