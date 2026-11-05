// Unit tests of what makes a noise and when it stops (MAYAP_INDUSTRIAL_v1_0_0/alarm_sound.h), the code MachineController runs every cycle.
// 2.9.5: the over-temperature lines have ONE rule - the siren sounds while the alarm is on and is silent the moment it is off. (Versions before waited 180 s
// and started the wait again at every dip, so a reading that flickered around the line never made a sound.)
#include "../MAYAP_INDUSTRIAL_v1_0_0/alarm_sound.h"
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>

using namespace alarmsound;

struct Rig {
  LoudVoice v;
  LoudVoice::Params p;
  uint32_t t0;
  explicit Rig(uint32_t start = 1000U) : t0(start) {}
  uint32_t ms(int s) const { return t0 + static_cast<uint32_t>(s) * 1000UL; }
  void at(int s, float pv, bool active = true) { v.update(ms(s), true, pv, active, p); }
};

int main() {
  // 1. The alarm on: the siren sounds at once, continuously at first.
  {
    Rig r;
    r.at(0, 37.6f, false);
    assert(!r.v.sounding() && !r.v.siren(r.ms(0), r.p));
    r.at(1, 37.7f);
    assert(r.v.sounding() && r.v.audible() && r.v.siren(r.ms(1), r.p));
    assert(r.v.siren(r.ms(250), r.p));
    std::printf("alarm on: siren at once\n");
  }
  // 2. The alarm off: silent at once, and a new alarm sounds at once again (nothing is remembered, nothing waits).
  {
    Rig r;
    r.at(0, 37.8f); r.at(10, 37.8f);
    assert(r.v.audible());
    r.at(11, 37.69f, false);
    assert(!r.v.audible() && !r.v.sounding() && !r.v.siren(r.ms(11), r.p));
    r.at(20, 37.75f);
    assert(r.v.audible() && r.v.siren(r.ms(20), r.p));
  }
  // 3. A reading that flickers around the line: whenever the alarm is on the siren is on (it does not start a wait at every dip).
  {
    Rig r;
    for (int s = 0; s < 120; s += 2) {
      const bool on = (s / 2) % 3 != 2;                                        // on, on, off ...
      r.at(s, on ? 37.72f : 37.68f, on);
      assert(r.v.audible() == on);
    }
  }
  // 4. Continuous for 5 minutes after it starts, then pulsed 5 s on / 25 s off.
  {
    Rig r;
    r.at(0, 37.8f); r.at(1, 37.8f);
    assert(r.v.sounding());
    assert(r.v.siren(r.ms(299), r.p));
    assert(r.v.siren(r.ms(301), r.p) && r.v.siren(r.ms(304), r.p));
    assert(!r.v.siren(r.ms(306), r.p) && !r.v.siren(r.ms(329), r.p));
    assert(r.v.siren(r.ms(330), r.p) && r.v.siren(r.ms(334), r.p) && !r.v.siren(r.ms(335), r.p));
  }
  // 5. ACK at the machine mutes; a falling reading keeps it quiet, a reading that stands comes back after 2 minutes, one that rises comes back at once,
  //    and 10 minutes is the longest silence.
  {
    Rig r;
    r.at(0, 37.9f); r.at(10, 37.9f);
    r.v.ack(r.ms(190), 37.9f);
    assert(r.v.muted() && !r.v.audible() && !r.v.siren(r.ms(190), r.p));
    r.at(240, 37.88f);
    assert(r.v.muted());
    r.at(311, 37.79f);                                                         // fell 0.11 over the check: quiet, look again later
    assert(r.v.muted());
    r.at(432, 37.78f);                                                         // did not fall another 0.1 over the next 2 minutes: back
    assert(!r.v.muted() && r.v.audible());
    Rig s;
    s.at(0, 37.9f); s.at(10, 37.9f);
    s.v.ack(s.ms(190), 37.9f);
    s.at(210, 38.11f);
    assert(!s.v.muted() && s.v.audible());                                     // rose 0.2
    Rig u;
    u.at(0, 37.95f); u.at(10, 37.95f);
    u.v.ack(u.ms(190), 37.95f);
    for (int sec = 191; sec < 900; sec += 10) u.at(sec, 37.95f - 0.0003f * static_cast<float>(sec - 190));   // falls slowly, never unmuted by the check
    assert(!u.v.muted());                                                      // ... but the 10-minute limit always ends it
  }
  // 6. The sensor lost: silence now (the sensor faults have their own voice).
  {
    Rig r;
    r.at(0, 37.9f); r.at(10, 37.9f);
    assert(r.v.audible());
    r.v.update(r.ms(11), false, NAN, true, r.p);
    assert(!r.v.audible());
  }
  // 7. The 32-bit clock wraps.
  {
    Rig r(0xFFFFFFFFUL - 100000UL);
    r.at(0, 37.8f); r.at(100, 37.8f);
    assert(r.v.siren(r.ms(100), r.p));
    assert(r.v.siren(r.ms(100 + 200), r.p) && !r.v.siren(r.ms(100 + 310), r.p));
    r.at(500, 37.45f, false);
    assert(!r.v.audible());
  }

  // 9. The Chime class: a chime for every code that appears, a thin reminder (here 30 minutes) while one lasts, silent when none is left.
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
  // 9. 2.9.9, the SirenGate. A reading standing on the High line makes the alarm itself flip at every sensor step; the siren must start ONCE.
  {
    SirenGate g; SirenGate::Params gp;
    const float line = 37.7f;
    uint32_t t = 1000U; int starts = 0; bool was = false; int offWhileFlicker = 0;
    for (int cycle = 0; cycle < 60; ++cycle) {                              // 15 minutes of 10 s over / 5 s under
      for (int s = 0; s < 15; ++s) {
        const bool active = s < 10;
        const bool on = g.update(t, true, active ? 37.7f : 37.6f, line, active, gp);
        if (on && !was) ++starts;
        if (!on && was) ++offWhileFlicker;
        was = on; t += 1000U;
      }
    }
    assert(starts == 1 && offWhileFlicker == 0 && g.on());                  // one start, never off in between
    for (int s = 0; s < 20; ++s) { assert(g.update(t, true, 37.5f, line, false, gp)); t += 1000U; }   // quiet 25 s so far (5 s ended the cycle): still on
    for (int s = 0; s < 8; ++s) { g.update(t, true, 37.5f, line, false, gp); t += 1000U; }
    assert(!g.on());                                                        // quiet over 30 s: off
    std::printf("siren gate: a flickering reading starts the siren once\n");
  }
  {   // a reading hovering a step over the line for an hour (the case of 04/11: silent for an hour before 2.9.5) sounds within two minutes
    SirenGate g; SirenGate::Params gp; uint32_t t = 1000U; int firstOn = -1;
    for (int s = 0; s < 3600; ++s) {
      const bool active = (s % 60) < 55;
      if (g.update(t, true, active ? 37.7f : 37.6f, 37.7f, active, gp) && firstOn < 0) firstOn = s;
      t += 1000U;
    }
    assert(firstOn >= 0 && firstOn <= 120 && g.on());
    std::printf("siren gate: hovering over the line sounds after %d s and stays\n", firstOn);
  }
  {   // clearly over: at once; a short touch: never
    SirenGate g; SirenGate::Params gp; uint32_t t = 1000U;
    assert(g.update(t, true, 37.82f, 37.7f, true, gp));                     // 0.12 over the line: at once
    SirenGate h; t = 1000U; bool any = false;
    for (int s = 0; s < 10; ++s) { any |= h.update(t, true, 37.7f, 37.7f, true, gp); t += 1000U; }
    for (int s = 0; s < 120; ++s) { any |= h.update(t, true, 37.6f, 37.7f, false, gp); t += 1000U; }
    assert(!any);                                                           // ten seconds on the line and gone: no siren
    SirenGate x; assert(!x.update(t, false, NAN, 37.7f, true, gp) && !x.on());   // no valid reading: silent
    std::printf("siren gate: well over = at once, a short touch = silent\n");
  }
  // 10. 2.9.9, ACK with the heater already off: the oven cannot fall on a hot day, the siren must not come back every two minutes.
  {
    Rig r;
    r.at(0, 37.8f); r.at(1, 37.8f);
    r.v.ack(r.ms(1), 37.8f);
    assert(!r.v.audible());
    for (int s = 2; s <= 500; ++s) r.v.update(r.ms(s), true, 37.8f, true, r.p, true);        // flat, heater off: 8 minutes
    assert(!r.v.audible());
    r.v.update(r.ms(501), true, 38.02f, true, r.p, true);                                    // rose 0.2: it speaks
    assert(r.v.audible());
    Rig q;                                                                                   // the same with the heater on: back after the two minute check
    q.at(0, 37.8f); q.at(1, 37.8f); q.v.ack(q.ms(1), 37.8f);
    for (int s = 2; s <= 130; ++s) q.v.update(q.ms(s), true, 37.8f, true, q.p, false);
    assert(q.v.audible());
    Rig m;                                                                                   // heater off, flat, and the time limit of ten minutes: it speaks
    m.at(0, 37.8f); m.at(1, 37.8f); m.v.ack(m.ms(1), 37.8f);
    for (int s = 2; s <= 700; ++s) m.v.update(m.ms(s), true, 37.8f, true, m.p, true);
    assert(m.v.audible());
    std::printf("ACK with the heater off: quiet until a rise of 0.2 or ten minutes\n");
  }
  std::printf("alarm-sound: OK\n");
  return 0;
}
