// Unit tests of what makes a noise and when it stops (MAYAP_INDUSTRIAL_v1_0_0/alarm_sound.h), the code MachineController runs every cycle.
// The over-temperature lines are loud at once (the siren sounds the moment the line is crossed) and stop when the reading is 0.2 degC under it.
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
  // 1. The owner's lines (SV 37.5, High 37.7): the siren sounds the moment the line is crossed, with no soft note first.
  {
    Rig r;
    r.at(0, 37.6f, 37.7f, false);                                              // under the line: the line is not active yet
    assert(!r.v.sounding());
    r.at(1, 37.7f, 37.7f);
    assert(r.v.sounding() && r.v.audible() && r.v.siren(r.ms(1), r.p));
    assert(r.v.siren(r.ms(120), r.p) && r.v.siren(r.ms(299), r.p));            // continuous for the first minutes
    std::printf("line crossed: siren at once\n");
  }
  // 2. ... and it stops when the reading has fallen 0.2 degC (2 s to confirm), not before.
  {
    Rig r;
    r.at(0, 37.75f, 37.7f);
    assert(r.v.audible());
    r.at(5, 37.55f, 37.7f);                                                    // 0.15 under: still loud
    assert(r.v.audible());
    r.at(6, 37.5f, 37.7f);                                                     // 0.2 under
    assert(r.v.audible());                                                     // the confirm time has not passed
    r.at(7, 37.5f, 37.7f);
    assert(r.v.audible());
    r.at(8, 37.49f, 37.7f);
    assert(!r.v.audible() && !r.v.sounding() && !r.v.siren(r.ms(8), r.p));
    std::printf("fell 0.2: silent\n");
  }
  // 3. A reading that wanders between the line and 0.2 under it does not make the siren chatter: once silent it stays silent until the line is crossed again.
  {
    Rig r;
    r.at(0, 37.8f, 37.7f);
    r.at(1, 37.4f, 37.7f); r.at(4, 37.4f, 37.7f);
    assert(!r.v.audible());
    r.at(5, 37.6f, 37.7f);                                                     // up again, but still under the line
    assert(!r.v.audible());
    r.at(6, 37.7f, 37.7f);                                                     // back at the line: loud again
    assert(r.v.audible());
    // the line itself clearing resets everything at once
    r.at(7, 37.7f, 37.7f, false);
    assert(!r.v.audible() && !r.v.sounding());
  }
  // 4. Continuous for 5 minutes, then pulsed 5 s on / 25 s off (a reading stuck over the line must not be a wall of noise for hours).
  {
    Rig r;
    r.at(0, 37.8f, 37.7f);
    assert(r.v.siren(r.ms(299), r.p));
    assert(r.v.siren(r.ms(301), r.p) && r.v.siren(r.ms(304), r.p));
    assert(!r.v.siren(r.ms(306), r.p) && !r.v.siren(r.ms(329), r.p));
    assert(r.v.siren(r.ms(330), r.p) && r.v.siren(r.ms(334), r.p) && !r.v.siren(r.ms(335), r.p));
  }
  // 5. ACK at the machine mutes; a falling reading keeps it quiet, a reading that stands comes back after 2 minutes, one that rises comes back at once,
  //    and 10 minutes is the longest silence.
  {
    Rig r;
    r.at(0, 37.9f, 37.7f);
    r.v.ack(r.ms(10), 37.9f);
    assert(r.v.muted() && !r.v.audible() && !r.v.siren(r.ms(10), r.p));
    r.at(60, 37.88f, 37.7f);
    assert(r.v.muted());
    r.at(131, 37.79f, 37.7f);                                                  // fell 0.11 over the check: quiet, look again later
    assert(r.v.muted());
    r.at(252, 37.78f, 37.7f);                                                  // did not fall another 0.1 over the next 2 minutes: back
    assert(!r.v.muted() && r.v.audible());
    Rig s;
    s.at(0, 37.9f, 37.7f);
    s.v.ack(s.ms(10), 37.9f);
    s.at(30, 38.11f, 37.7f);
    assert(!s.v.muted() && s.v.audible());                                     // rose 0.2
    Rig u;
    u.at(0, 37.95f, 37.7f);
    u.v.ack(u.ms(10), 37.95f);
    for (int sec = 11; sec < 700; sec += 10) u.at(sec, 37.95f - 0.0004f * static_cast<float>(sec), 37.7f);   // falls slowly, never unmuted by the check
    assert(!u.v.muted());                                                      // ... but the 10-minute limit always ends it
  }
  // 6. The sensor lost: silence now (the sensor faults have their own voice).
  {
    Rig r;
    r.at(0, 37.9f, 37.7f);
    r.v.update(r.ms(1), false, NAN, 37.7f, true, r.p);
    assert(!r.v.audible());
  }
  // 7. The Emergency line is the same voice: loud at once, quiet 0.2 under (the default lines 38.2 / 39.0 behave alike).
  {
    Rig r;
    r.at(0, 39.0f, 39.0f);
    assert(r.v.siren(r.ms(0), r.p));
    r.at(10, 38.79f, 39.0f); r.at(13, 38.79f, 39.0f);
    assert(!r.v.audible());
  }
  // 8. The 32-bit clock wraps.
  {
    Rig r(0xFFFFFFFFUL - 100000UL);
    r.at(0, 37.8f, 37.7f);
    assert(r.v.siren(r.ms(100), r.p) && r.v.siren(r.ms(299), r.p) && !r.v.siren(r.ms(310), r.p));
    r.at(310, 37.45f, 37.7f); r.at(313, 37.45f, 37.7f);
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
