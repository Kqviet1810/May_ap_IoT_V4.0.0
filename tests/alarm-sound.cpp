// Unit tests of what makes a noise and when it stops (MAYAP_INDUSTRIAL_v1_0_0/alarm_sound.h), the code MachineController runs every cycle.
// The over-temperature lines wait a 180 s grace time (the fan brings the reading down), sound only if the reading is still over the line, at once when 0.5 degC over, and stop as soon as the reading is under the line.
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
  void at(int s, float pv, float line, bool active = true) { v.update(ms(s), true, pv, line, active, p); }
};

int main() {
  // Helper: hold the reading over the line from second 0 to `to` so the 180 s grace time passes, then the siren sounds.
  // 1. A line crossed does NOT sound at once (the first-climb overshoot is brought down by the fan): silent during the grace time, loud after it if the
  //    reading is still at or over the line.
  {
    Rig r;
    r.at(0, 37.6f, 37.7f, false);
    assert(!r.v.sounding());
    r.at(1, 37.7f, 37.7f);
    assert(!r.v.sounding() && !r.v.audible() && !r.v.siren(r.ms(1), r.p));
    for (int s = 2; s < 181; s += 5) r.at(s, 37.75f, 37.7f);                  // 3 minutes over the line, fan running, not under yet
    assert(!r.v.audible());                                                    // 180 s after the crossing (second 1) is second 181
    r.at(181, 37.75f, 37.7f);
    assert(r.v.sounding() && r.v.audible() && r.v.siren(r.ms(181), r.p));
    assert(r.v.siren(r.ms(300), r.p));                                         // continuous at first
    std::printf("line crossed: grace, then siren\n");
  }
  // 2. A false alarm: the reading falls under the line inside the grace time: no sound at all, and the grace time starts again at the next crossing.
  {
    Rig r;
    r.at(0, 37.8f, 37.7f);
    r.at(60, 37.75f, 37.7f);
    r.at(100, 37.69f, 37.7f); r.at(103, 37.69f, 37.7f);                        // under the line for 2 s: forgotten
    assert(!r.v.sounding());
    r.at(110, 37.72f, 37.7f);                                                  // over again: a new grace time from here
    r.at(250, 37.72f, 37.7f);
    assert(!r.v.audible());                                                    // 140 s since the new crossing
    r.at(291, 37.72f, 37.7f);
    assert(r.v.audible());
  }
  // 3. Far over the line (panicC 0.5): at once, whatever the grace time says.
  {
    Rig r;
    r.at(0, 38.15f, 37.7f);
    assert(!r.v.audible());
    r.at(1, 38.2f, 37.7f);
    assert(r.v.audible() && r.v.siren(r.ms(1), r.p));
  }
  // 4. Once loud, it stops as soon as the reading is under the line (2 s to confirm) and stays silent until a new crossing has lived through its grace time.
  {
    Rig r;
    r.at(0, 37.9f, 37.7f); r.at(181, 37.9f, 37.7f);
    assert(r.v.audible());
    r.at(200, 37.69f, 37.7f);
    assert(r.v.audible());                                                     // confirm time not passed
    r.at(202, 37.69f, 37.7f);
    assert(!r.v.audible() && !r.v.sounding() && !r.v.siren(r.ms(202), r.p));
    r.at(210, 37.7f, 37.7f);                                                   // back at the line: grace starts, silent
    assert(!r.v.audible());
    // the line itself clearing resets everything at once
    r.at(211, 37.7f, 37.7f, false);
    assert(!r.v.audible() && !r.v.sounding());
  }
  // 5. Continuous for 5 minutes after it starts, then pulsed 5 s on / 25 s off.
  {
    Rig r;
    r.at(0, 37.8f, 37.7f); r.at(180, 37.8f, 37.7f);
    assert(r.v.sounding());
    assert(r.v.siren(r.ms(180 + 299), r.p));
    assert(r.v.siren(r.ms(180 + 301), r.p) && r.v.siren(r.ms(180 + 304), r.p));
    assert(!r.v.siren(r.ms(180 + 306), r.p) && !r.v.siren(r.ms(180 + 329), r.p));
    assert(r.v.siren(r.ms(180 + 330), r.p) && r.v.siren(r.ms(180 + 334), r.p) && !r.v.siren(r.ms(180 + 335), r.p));
  }
  // 6. ACK at the machine mutes; a falling reading keeps it quiet, a reading that stands comes back after 2 minutes, one that rises comes back at once,
  //    and 10 minutes is the longest silence.
  {
    Rig r;
    r.at(0, 37.9f, 37.7f); r.at(181, 37.9f, 37.7f);
    r.v.ack(r.ms(190), 37.9f);
    assert(r.v.muted() && !r.v.audible() && !r.v.siren(r.ms(190), r.p));
    r.at(240, 37.88f, 37.7f);
    assert(r.v.muted());
    r.at(311, 37.79f, 37.7f);                                                  // fell 0.11 over the check: quiet, look again later
    assert(r.v.muted());
    r.at(432, 37.78f, 37.7f);                                                  // did not fall another 0.1 over the next 2 minutes: back
    assert(!r.v.muted() && r.v.audible());
    Rig s;
    s.at(0, 37.9f, 37.7f); s.at(181, 37.9f, 37.7f);
    s.v.ack(s.ms(190), 37.9f);
    s.at(210, 38.11f, 37.7f);
    assert(!s.v.muted() && s.v.audible());                                     // rose 0.2
    Rig u;
    u.at(0, 37.95f, 37.7f); u.at(181, 37.95f, 37.7f);
    u.v.ack(u.ms(190), 37.95f);
    for (int sec = 191; sec < 900; sec += 10) u.at(sec, 37.95f - 0.0003f * static_cast<float>(sec - 190), 37.7f);   // falls slowly, never unmuted by the check
    assert(!u.v.muted());                                                      // ... but the 10-minute limit always ends it
  }
  // 7. The sensor lost: silence now (the sensor faults have their own voice).
  {
    Rig r;
    r.at(0, 37.9f, 37.7f); r.at(181, 37.9f, 37.7f);
    assert(r.v.audible());
    r.v.update(r.ms(182), false, NAN, 37.7f, true, r.p);
    assert(!r.v.audible());
  }
  // 8. The Emergency line is the same voice (default lines 38.2 / 39.0): grace time, panic at 39.5, quiet under the line.
  {
    Rig r;
    r.at(0, 39.0f, 39.0f);
    assert(!r.v.audible());
    r.at(181, 39.0f, 39.0f);
    assert(r.v.siren(r.ms(181), r.p));
    r.at(190, 38.99f, 39.0f); r.at(193, 38.99f, 39.0f);
    assert(!r.v.audible());
    Rig q;
    q.at(0, 39.5f, 39.0f);
    assert(q.v.audible());
  }
  // 8b. The 32-bit clock wraps.
  {
    Rig r(0xFFFFFFFFUL - 100000UL);
    r.at(0, 37.8f, 37.7f); r.at(100, 37.8f, 37.7f);
    assert(!r.v.audible());
    r.at(181, 37.8f, 37.7f);
    assert(r.v.siren(r.ms(181 + 100), r.p) && !r.v.siren(r.ms(181 + 310), r.p));
    r.at(500, 37.45f, 37.7f); r.at(503, 37.45f, 37.7f);
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
  std::printf("alarm-sound: OK\n");
  return 0;
}
