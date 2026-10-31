// Unit tests of what makes a noise and when it stops (MAYAP_INDUSTRIAL_v1_0_0/alarm_sound.h), the code MachineController runs every cycle.
// The owner may set the High / Emergency lines a tenth of a degree over the vent line; the loud siren must still be reserved for a real danger.
#include "../MAYAP_INDUSTRIAL_v1_0_0/alarm_sound.h"
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>

using namespace alarmsound;

static const float SP = 37.5f;

struct Rig {
  EmergencyVoice v;
  EmergencyVoice::Params p;
  uint32_t t0;
  explicit Rig(uint32_t start = 1000U) : t0(start) {}
  uint32_t ms(int s) const { return t0 + static_cast<uint32_t>(s) * 1000UL; }
  void at(int s, float pv, float line, bool active = true) { v.update(ms(s), true, pv, SP, line, active, p); }
};

int main() {
  // 1. A tight line (37.8 on SV 37.5): crossing it gives a quiet note, not the siren.
  {
    Rig r;
    r.at(0, 37.7f, 37.8f, false);                                              // under the line: the line is not active yet
    assert(!r.v.stage() && !r.v.consumeChime());
    r.at(1, 37.8f, 37.8f);
    assert(!r.v.stage() && r.v.consumeChime() && !r.v.consumeChime());       // one note
    for (int s = 2; s < 150; ++s) { r.at(s, 37.82f, 37.8f); assert(!r.v.stage()); }
    r.at(181, 37.82f, 37.8f);                                                // 180 s over the line and not coming down: now the siren
    assert(r.v.stage() && r.v.audible() && r.v.siren(r.ms(181), r.p));
    std::printf("tight line: note at once, siren after 180 s\n");
  }
  // 2. ... or at once when it climbs on (0.3 over where it crossed), or when it is a real danger (SV + 0.8) whatever the line.
  {
    Rig r;
    r.at(0, 37.8f, 37.8f);
    assert(!r.v.stage());
    r.at(20, 38.05f, 37.8f);
    assert(!r.v.stage());
    r.at(21, 38.11f, 37.8f);
    assert(r.v.stage());
    Rig q;
    q.at(0, 38.31f, 37.8f);
    assert(q.v.stage() && !q.v.consumeChime());                              // already a danger: no soft note first
  }
  // 3. The default line (39.0): the siren at once, as before.
  {
    Rig r;
    r.at(0, 38.9f, 39.0f, false);
    assert(!r.v.stage());
    r.at(1, 39.0f, 39.0f);
    assert(r.v.stage() && r.v.siren(r.ms(1), r.p) && !r.v.consumeChime());
  }
  // 4. The siren is continuous for a minute, then pulsed 5 s on / 25 s off.
  {
    Rig r;
    r.at(0, 39.2f, 39.0f);
    assert(r.v.stage());
    assert(r.v.siren(r.ms(30), r.p) && r.v.siren(r.ms(59), r.p));
    assert(r.v.siren(r.ms(61), r.p) && r.v.siren(r.ms(64), r.p));
    assert(!r.v.siren(r.ms(66), r.p) && !r.v.siren(r.ms(89), r.p));
    assert(r.v.siren(r.ms(90), r.p) && r.v.siren(r.ms(94), r.p) && !r.v.siren(r.ms(95), r.p));
  }
  // 5. Cooled by 0.1 under the line for 2 s: silence at once, though the lock of the heater (the Emergency line itself) is still there. It does not
  //    speak again for a reading that merely hovers at the line; it does for one that climbs 0.1 over it.
  {
    Rig r;
    r.at(0, 39.3f, 39.0f);
    assert(r.v.audible());
    r.at(5, 38.85f, 39.0f);
    assert(r.v.audible());                                                   // 2 s have not passed
    r.at(7, 38.85f, 39.0f);
    assert(!r.v.audible() && !r.v.stage());
    r.at(8, 39.02f, 39.0f);
    assert(!r.v.audible());                                                  // hovering: quiet
    r.at(9, 39.12f, 39.0f);
    assert(r.v.audible());                                                   // climbing again: the siren
    // the line itself clearing resets everything at once
    r.at(10, 39.12f, 39.0f, false);
    assert(!r.v.audible() && !r.v.stage());
  }
  // 6. ACK at the machine mutes; a falling reading keeps it quiet, a reading that stands comes back after 2 minutes, one that rises comes back at once.
  {
    Rig r;
    r.at(0, 39.3f, 39.0f);
    r.v.ack(r.ms(10), 39.3f);
    assert(r.v.muted() && !r.v.audible() && !r.v.siren(r.ms(10), r.p));
    r.at(60, 39.28f, 39.0f);
    assert(r.v.muted());
    r.at(131, 39.19f, 39.0f);                                                // fell 0.11 over the check: quiet, look again later
    assert(r.v.muted());
    r.at(252, 39.18f, 39.0f);                                                // did not fall another 0.1 over the next 2 minutes: back
    assert(!r.v.muted() && r.v.audible());
    Rig s;
    s.at(0, 39.3f, 39.0f);
    s.v.ack(s.ms(10), 39.3f);
    s.at(30, 39.51f, 39.0f);
    assert(!s.v.muted() && s.v.audible());                                   // rose 0.2
    Rig u;
    u.at(0, 39.3f, 39.0f);
    u.v.ack(u.ms(10), 39.3f);
    for (int sec = 11; sec < 700; sec += 10) u.at(sec, 39.3f - 0.0005f * static_cast<float>(sec), 39.0f);   // falls slowly, 0.1 in 200 s: never unmuted by the check
    assert(!u.v.muted());                                                    // ... but the 10-minute limit always ends it
  }
  // 7. The sensor lost: silence now (the sensor faults have their own voice).
  {
    Rig r;
    r.at(0, 39.3f, 39.0f);
    r.v.update(r.ms(1), false, NAN, SP, 39.0f, true, r.p);
    assert(!r.v.audible());
  }
  // 8. The 32-bit clock wraps.
  {
    Rig r(0xFFFFFFFFUL - 100000UL);
    r.at(0, 37.8f, 37.8f);
    assert(r.v.consumeChime());
    r.at(179, 37.82f, 37.8f);
    assert(!r.v.stage());
    r.at(181, 37.82f, 37.8f);
    assert(r.v.stage());
  }

  // 9. The High line: three beeps, then silence; it speaks again only when it gets worse, never more often than every 5 minutes.
  {
    HighVoice h; HighVoice::Params p;
    uint32_t t = 0U;
    auto tick = [&](int s, float pv, bool active = true) { t = 1000U + static_cast<uint32_t>(s) * 1000UL; h.update(t, true, pv, active, p); };
    tick(0, 37.7f);
    assert(h.consumeAlert() && !h.consumeAlert());
    for (int s = 1; s < 300; ++s) { tick(s, 37.72f); assert(!h.consumeAlert()); }          // standing there: silent for the first minutes
    tick(301, 37.95f);
    assert(h.consumeAlert());                                                              // 0.25 worse and 5 minutes have passed
    tick(310, 38.3f);
    assert(!h.consumeAlert());                                                             // worse again, but not within 5 minutes of the last note
    tick(602, 38.3f);
    assert(h.consumeAlert());                                                              // (rise) the gap is over
    for (int s = 603; s < 1300; s += 5) { tick(s, 38.3f - 0.0002f * static_cast<float>(s - 602)); }   // 0.14 in 700 s: coming down
    assert(!h.consumeAlert());
    // a standing reading: 10 minutes without a fall of 0.1 is a note
    HighVoice g;
    auto tg = [&](int s, float pv, bool active = true) { g.update(1000U + static_cast<uint32_t>(s) * 1000UL, true, pv, active, p); };
    tg(0, 37.7f); assert(g.consumeAlert());
    for (int s = 1; s < 598; ++s) tg(s, 37.7f);
    assert(!g.consumeAlert());
    tg(601, 37.7f);
    assert(g.consumeAlert());
    // ACK keeps it silent until the condition is gone; a new episode speaks again
    HighVoice k;
    auto tk = [&](int s, float pv, bool active = true) { k.update(1000U + static_cast<uint32_t>(s) * 1000UL, true, pv, active, p); };
    tk(0, 37.7f); assert(k.consumeAlert());
    k.ack();
    tk(400, 38.5f);
    assert(!k.consumeAlert());
    tk(401, 38.5f, false);
    tk(500, 37.7f);
    assert(k.consumeAlert());
    std::printf("High: one note, then only when it gets worse\n");
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
  std::printf("alarm-sound: OK\n");
  return 0;
}
