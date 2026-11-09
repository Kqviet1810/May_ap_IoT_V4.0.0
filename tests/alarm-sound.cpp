// Unit tests of what makes a noise and when it stops (MAYAP_INDUSTRIAL_v1_0_0/alarm_sound.h), the code MachineController runs every cycle.
// 2.9.11: the over-temperature siren is ONE rule measured from the setpoint (OverSetpointSiren); no ACK, no pulsing, no special level.
#include "../MAYAP_INDUSTRIAL_v1_0_0/alarm_sound.h"
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>

using namespace alarmsound;

struct Rig {
  OverSetpointSiren s;
  OverSetpointSiren::Params p;
  bool at(uint32_t sec, float over, bool eligible = true) { return s.update(1000U + sec * 1000UL, over, eligible, p); }
};

int main() {
  // 1. 0.2 over for 60 s: silent until the 60th second, then on.
  {
    Rig r;
    for (uint32_t t = 0; t < 60; ++t) assert(!r.at(t, 0.2f));
    assert(r.at(60, 0.2f));
    std::printf("0.2 over for 60 s -> siren\n");
  }
  // 2. 0.3 over: at once. 0.29 is not.
  {
    Rig r;
    assert(!r.at(0, 0.29f));
    assert(r.at(1, 0.3f));
    Rig q;
    assert(q.at(0, 0.4f));
  }
  // 3. A dip under 0.2 restarts the 60 s (it is an unbroken stretch).
  {
    Rig r;
    for (uint32_t t = 0; t < 40; ++t) r.at(t, 0.2f);
    r.at(40, 0.1f);
    for (uint32_t t = 41; t < 100; ++t) assert(!r.at(t, 0.2f));
    assert(r.at(101, 0.2f));
  }
  // 4. Continuous: on for 30 minutes at 0.25 over, never pulsed, no ACK exists.
  {
    Rig r;
    r.at(0, 0.3f);
    for (uint32_t t = 1; t < 1800; ++t) assert(r.at(t, 0.25f));
  }
  // 5. Off only after 60 s at/under 0.1 over; between 0.1 and 0.2 it stays on; a rise during the 60 s restarts them.
  {
    Rig r;
    r.at(0, 0.5f);
    for (uint32_t t = 1; t < 100; ++t) assert(r.at(t, 0.15f));
    for (uint32_t t = 100; t < 160; ++t) assert(r.at(t, 0.1f));
    assert(!r.at(160, 0.1f));
    // on again by the same rule
    assert(!r.at(161, 0.2f));
    assert(r.at(221, 0.2f));
  }
  {
    Rig r;
    r.at(0, 0.5f);
    for (uint32_t t = 1; t < 50; ++t) r.at(t, 0.0f);
    r.at(50, 0.15f);                                          // the calm stretch is broken
    for (uint32_t t = 51; t < 100; ++t) assert(r.at(t, 0.0f));
    assert(!r.at(111, 0.0f));
  }
  // 6. A sample that is not valid keeps the state, both ways: a siren that sounds goes on, one that is waiting does not start or reset by it.
  {
    Rig r;
    r.at(0, 0.5f);
    for (uint32_t t = 1; t < 300; ++t) assert(r.at(t, NAN));
    Rig q;
    for (uint32_t t = 0; t < 30; ++t) q.at(t, 0.2f);
    for (uint32_t t = 30; t < 200; ++t) assert(!q.at(t, NAN));
    assert(!q.s.on());
  }
  // 7. Not eligible (no batch, the alarm without batch is off): silent and forgotten.
  {
    Rig r;
    r.at(0, 0.5f);
    assert(!r.at(1, 0.5f, false));
    assert(r.at(2, 0.5f, true));
  }
  // 8. The 32-bit clock wraps.
  {
    OverSetpointSiren s; OverSetpointSiren::Params p;
    uint32_t t = 0xFFFFFFFFUL - 30000UL;
    bool on = false;
    for (int i = 0; i < 70; ++i) { on = s.update(t, 0.2f, true, p); t += 1000UL; }
    assert(on);
  }
  // 9. A reading on the edge as a float (37.7 - 37.5 is 0.2000008, 37.6 - 37.4 is 0.19999695) counts as 0.2.
  {
    Rig r;
    for (uint32_t t = 0; t < 60; ++t) r.at(t, 37.6f - 37.4f);
    assert(r.at(60, 37.6f - 37.4f));
  }

  // 10. The Chime class: a chime for every code that appears, a thin reminder (here 30 minutes) while one lasts, silent when none is left.
  {
    Reminder r;
    const uint32_t remind = 1800000UL;
    uint32_t t = 0U;
    assert(!r.update(t, 0U, remind));
    assert(r.update(t, 1ULL << 3, remind));                                    // appears
    int chimes = 0;
    for (int s = 1; s <= 3600; ++s) { t = static_cast<uint32_t>(s) * 1000UL; if (r.update(t, 1ULL << 3, remind)) ++chimes; }
    assert(chimes == 2);                                                       // 30 and 60 minutes
    t += 1000U;
    assert(r.update(t, (1ULL << 3) | (1ULL << 9), remind));                    // another code appears: a chime now
    t += 1000U;
    assert(!r.update(t, (1ULL << 3) | (1ULL << 9), remind));
    t += 1000U;
    assert(!r.update(t, 0U, remind));                                          // all gone
    t += 1000U;
    assert(r.update(t, 1ULL << 3, remind));                                    // and back: a chime again
  }
  std::printf("alarm_sound ok\n");
  return 0;
}
