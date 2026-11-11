// Unit tests of the vent line (MAYAP_INDUSTRIAL_v1_0_0/vent_line.h), the code MachineController runs every cycle: when the exhaust fans of the
// overheat run. The two small fans take little heat away and their contactor is rated for few operations: the line is a last resort. It must not
// chatter, must not start the fans for a reading that is already coming down, must not be blinded by a reading that flickers between two sensor
// steps, and must start them when the heater has nothing left to give up.
#include "../MAYAP_INDUSTRIAL_v1_0_0/vent_line.h"
#include <cassert>
#include <cmath>
#include <cstdio>

static const float ON = 37.6f, OFF = 37.5f;      // the vent threshold is one sensor step over SP (the field machine: 37.6 / 37.5)

struct Run {
  int firstOn = -1, firstOff = -1, starts = 0;
  bool budget = false, deferred = false, maxRun = false;
  uint8_t why = 0;
  float tokensAfter = 0.0f;
};

// One reading a second, seconds [0, seconds]; temp(t) the reading, ctx(t) the context. The ring needs 20 s of readings before it decides, so the
// scenarios give the line a quiet lead-in (`lead` s of 37.5) before the interesting part, as the oven would have.
template <class T, class C>
static Run run(VentLine &v, const VentLine::Params &p, uint32_t startMs, int seconds, T temp, C ctx) {
  Run r;
  bool was = v.active();
  for (int t = 0; t <= seconds; ++t) {
    const bool a = v.update(startMs + static_cast<uint32_t>(t) * 1000UL, true, temp(t), ON, OFF, p, ctx(t));
    if (a && !was) { ++r.starts; if (r.firstOn < 0) { r.firstOn = t; r.why = v.why(); } }
    if (!a && was && r.firstOff < 0) r.firstOff = t;
    if (v.bits() & VentLine::BitBudget) r.budget = true;
    if (v.bits() & VentLine::BitDeferred) r.deferred = true;
    if (v.bits() & VentLine::BitMaxRun) r.maxRun = true;
    was = a;
  }
  r.tokensAfter = v.tokens();
  return r;
}
static VentLine::Ctx quiet(int) { return VentLine::Ctx(); }
static VentLine::Ctx tau150(int) { VentLine::Ctx c; c.tauS = 150.0f; return c; }

int main() {
  const VentLine::Params p;

  // 1. A reading that stands one step over the threshold and does not fall: the watch lasts 1.2 x tau (180 s for 150 s), then the fans start.
  {
    VentLine v;
    const Run r = run(v, p, 1000U, 900, [](int t) { return t < 60 ? 37.5f : 37.6f; }, tau150);
    assert(r.firstOn >= 60 + 170 && r.firstOn <= 60 + 205);
    assert(r.why == VentLine::WhyStuck);
    std::printf("steady 37.6: the fans start %d s after the reading went over (stuck)\n", r.firstOn - 60);
  }
  // 2. A reading over the threshold that is coming down never starts them: it is back under before any watch runs out, and a fall of the mean by
  //    0.05 restarts the watch even while it is still over.
  {
    VentLine v;
    const Run r = run(v, p, 0U, 1200, [](int t) { return t < 60 ? 37.5f : (t < 150 ? 37.66f - 0.16f * static_cast<float>(t - 60) / 90.0f : 37.5f); }, tau150);
    assert(r.firstOn < 0);
    VentLine w;                                  // 0.05 lower every 100 s for 280 s: a watch of 180 s would run out once over, and none does
    const Run q = run(w, p, 0U, 1200, [](int t) { return t < 60 ? 37.5f : (t < 340 ? 37.64f - 0.14f * static_cast<float>(t - 60) / 280.0f : 37.5f); }, tau150);
    assert(q.firstOn < 0);
    std::printf("falling reading: the fans do not start\n");
  }
  // 3. A reading that flickers between two sensor steps (the true temperature is 37.57): the old rule never saw "a whole minute over the
  //    threshold"; the mean does. 7 of 10 readings are 37.6.
  {
    VentLine v;
    const Run r = run(v, p, 0U, 900, [](int t) { return t < 60 ? 37.5f : ((t % 10) < 7 ? 37.6f : 37.5f); }, tau150);
    assert(r.firstOn >= 60 + 170 && r.firstOn <= 60 + 215);
    // ... and a true temperature of 37.53 (3 of 10 readings 37.6) is not over: no fans, however long.
    VentLine w;
    const Run q = run(w, p, 0U, 3600, [](int t) { return t < 60 ? 37.5f : ((t % 10) < 3 ? 37.6f : 37.5f); }, tau150);
    assert(q.firstOn < 0);
    std::printf("flickering 37.5/37.6 (true 37.57): start %d s after, true 37.53: never\n", r.firstOn - 60);
  }
  // 4. A decisive overshoot (a further 0.1 over the threshold) starts them after the confirm time, long before the watch ends.
  {
    VentLine v;
    const Run r = run(v, p, 0U, 600, [](int t) { return t < 40 ? 37.5f : 37.7f; }, tau150);
    assert(r.firstOn >= 40 + 20 && r.firstOn <= 40 + 40);
    assert(r.why == VentLine::WhyFast);
    std::printf("37.7: the fans start %d s after (fast)\n", r.firstOn - 40);
  }
  // 5. The heater at its floor with the reading over: nothing is left to cut, so the fans start after 150 s, before a long watch (tau 220 -> 260 s).
  {
    VentLine v;
    const Run r = run(v, p, 0U, 900, [](int t) { return t < 60 ? 37.5f : 37.6f; },
                      [](int) { VentLine::Ctx c; c.tauS = 220.0f; c.heaterAvgPct = 1.0f; return c; });
    assert(r.firstOn >= 60 + 148 && r.firstOn <= 60 + 180);
    assert(r.why == VentLine::WhyFloor);
    VentLine w;                                                  // the same with a heater that still works hard: it waits the full window
    const Run q = run(w, p, 0U, 900, [](int t) { return t < 60 ? 37.5f : 37.6f; },
                      [](int) { VentLine::Ctx c; c.tauS = 220.0f; c.heaterAvgPct = 12.0f; return c; });
    assert(q.firstOn >= 60 + 255 && q.firstOn <= 60 + 285);
    assert(q.why == VentLine::WhyStuck);
    std::printf("heater at floor: %d s, heater working: %d s\n", r.firstOn - 60, q.firstOn - 60);
  }
  // 6. The prediction says more is on its way while the reading is over: start without waiting for the watch.
  {
    VentLine v;
    const Run r = run(v, p, 0U, 600, [](int t) { return t < 60 ? 37.5f : 37.6f; },
                      [](int t) { VentLine::Ctx c; c.tauS = 150.0f; c.predictedC = t >= 60 ? 37.9f : 37.5f; return c; });
    assert(r.firstOn >= 60 + 18 && r.firstOn <= 60 + 40);
    assert(r.why == VentLine::WhyPredicted);
    // A prediction alone, with the reading not over, does nothing.
    VentLine w;
    const Run q = run(w, p, 0U, 600, [](int) { return 37.5f; }, [](int) { VentLine::Ctx c; c.predictedC = 37.9f; return c; });
    assert(q.firstOn < 0);
  }
  // 6b. The prediction says the temperature is on its way DOWN (the cut of the heater is still travelling to the sensor): the stuck / floor starts wait,
  //     for at most two watch windows; a decisive overshoot is never held back.
  {
    VentLine v;
    const Run r = run(v, p, 0U, 1200, [](int t) { return t < 60 ? 37.5f : 37.6f; },
                      [](int) { VentLine::Ctx c; c.tauS = 150.0f; c.predictedC = 37.5f; c.heaterAvgPct = 1.0f; return c; });
    assert(r.firstOn >= 60 + 2 * 180 - 5 && r.firstOn <= 60 + 2 * 180 + 40);          // held back to the end of the second window, then it starts anyway
    VentLine w;
    const Run q = run(w, p, 0U, 1200, [](int t) { return t < 60 ? 37.5f : 37.6f; },
                      [](int) { VentLine::Ctx c; c.tauS = 150.0f; c.predictedC = 37.6f; return c; });   // a prediction that stays over: no hold
    assert(q.firstOn >= 60 + 170 && q.firstOn <= 60 + 205);
    VentLine z;
    const Run f = run(z, p, 0U, 400, [](int t) { return t < 40 ? 37.5f : 37.8f; },
                      [](int) { VentLine::Ctx c; c.tauS = 150.0f; c.predictedC = 37.5f; return c; });
    assert(f.firstOn >= 40 + 20 && f.firstOn <= 40 + 40);
    std::printf("prediction under the threshold: start held to %d s (second window), fast overshoot not held\n", r.firstOn - 60);
  }
  // 7. Run time: at least 3 minutes however fast the reading drops, at most 10 minutes however long it stays high; rest 5 minutes in between.
  {
    VentLine v;
    const Run r = run(v, p, 0U, 400, [](int t) { return t < 60 ? 37.5f : (t < 90 ? 37.7f : 37.4f); }, tau150);
    assert(r.firstOn >= 0 && r.firstOff - r.firstOn >= 180 && r.firstOff - r.firstOn <= 186);
    VentLine w;
    int on = -1, off = -1, again = -1;
    bool was = false;
    for (int t = 0; t < 2400; ++t) {
      const bool a = w.update(static_cast<uint32_t>(t) * 1000UL, true, t < 40 ? 37.5f : 37.8f, ON, OFF, p, tau150(t));
      if (a && !was) { if (on < 0) on = t; else if (again < 0) again = t; }
      if (!a && was && off < 0) off = t;
      was = a;
    }
    std::printf("on %d off %d again %d\n", on, off, again);
    assert(off - on >= 599 && off - on <= 602);                  // the longest run
    assert(again - off >= 299 && again - off <= 305);            // the rest
    std::printf("min run %d s, max run %d s, rest %d s\n", r.firstOff - r.firstOn, off - on, again - off);
  }
  // 8. The budget: three starts at once, then one every 30 minutes. A reading that stays high all day cannot wear the contactor out.
  {
    VentLine v;
    const Run r = run(v, p, 0U, 6 * 3600, [](int t) { return t < 40 ? 37.5f : 37.8f; }, tau150);
    assert(r.starts >= 3 + 9 && r.starts <= 3 + 12 + 1);         // 3 + one per 30 min over 6 h
    assert(r.budget);
    std::printf("six hours over: %d starts, budget blocked\n", r.starts);
  }
  // 9. The day profile already runs the fans: the line takes over without spending a token, and a profile block due within 5 minutes is waited for.
  {
    VentLine v;
    const Run r = run(v, p, 0U, 900, [](int t) { return t < 60 ? 37.5f : 37.6f; },
                      [](int) { VentLine::Ctx c; c.tauS = 150.0f; c.fanRunning = true; return c; });
    assert(r.firstOn >= 60 + 170 && r.firstOn <= 60 + 205);
    assert(std::fabs(r.tokensAfter - 3.0f) < 0.1f);              // nothing spent (the refill alone keeps it at the cap)
    VentLine w;
    // the profile block is due in 120 s after the watch ended and the reading is not decisive: wait, then take over when it runs
    int ownStart = -1;
    bool deferredSeen = false;
    for (int t = 0; t < 600; ++t) {
      VentLine::Ctx c; c.tauS = 150.0f;
      const int blockAt = 60 + 180 + 120;
      c.fanRunning = t >= blockAt;
      c.profileOnInS = t < blockAt ? static_cast<float>(blockAt - t) : NAN;
      const bool a = w.update(static_cast<uint32_t>(t) * 1000UL, true, t < 60 ? 37.5f : 37.6f, ON, OFF, p, c);
      if (w.bits() & VentLine::BitDeferred) deferredSeen = true;
      if (a && ownStart < 0) ownStart = t;
    }
    assert(deferredSeen);
    assert(ownStart >= 60 + 180 + 120 - 1 && ownStart <= 60 + 180 + 125);
    assert(std::fabs(w.tokens() - 3.0f) < 0.1f);
    // a decisive overshoot does not wait for the block
    VentLine z;
    const Run q = run(z, p, 0U, 400, [](int t) { return t < 40 ? 37.5f : 37.8f; },
                      [](int) { VentLine::Ctx c; c.tauS = 150.0f; c.profileOnInS = 60.0f; return c; });
    assert(q.firstOn >= 40 + 20 && q.firstOn <= 40 + 40);
    std::printf("profile running: takes over unpaid at %d s, waited for the block (%d s)\n", r.firstOn, ownStart);
  }
  // 10. A lost sensor value switches the line off at once and clears every wait.
  {
    VentLine v;
    bool on = false;
    for (int t = 0; t < 120; ++t) on = v.update(static_cast<uint32_t>(t) * 1000UL, true, t < 40 ? 37.5f : 37.8f, ON, OFF, p, tau150(t));
    assert(on);
    assert(!v.update(121000UL, false, NAN, ON, OFF, p, tau150(121)));
    assert(!v.active());
  }
  // 11. The 32-bit millisecond clock wraps: the same behaviour across the wrap.
  {
    VentLine v;
    const uint32_t start = 0xFFFFFFFFUL - 90000UL;
    const Run r = run(v, p, start, 900, [](int t) { return t < 60 ? 37.5f : 37.6f; }, tau150);
    assert(r.firstOn >= 60 + 170 && r.firstOn <= 60 + 205);
    VentLine w;
    const Run q = run(w, p, start, 900, [](int t) { return t < 40 ? 37.5f : 37.8f; }, quiet);
    assert(q.firstOn >= 40 + 20 && q.firstOn <= 40 + 40);
    assert(q.firstOff - q.firstOn >= 180);
  }
  // 12. A fine sensor (0.01 degC steps): the threshold in true terms is onTemp - 0.005 and everything scales with the step.
  {
    VentLine::Params f = p;
    f.stepC = 0.01f;
    VentLine v;
    const Run r = run(v, f, 0U, 900, [](int t) { return t < 60 ? 37.5f : 37.6f; }, tau150);
    assert(r.firstOn >= 60 + 170 && r.firstOn <= 60 + 225);   // the reading is only 0.005 over the true threshold: the mean needs almost the whole 30 s to get there
    VentLine w;
    const Run q = run(w, f, 0U, 900, [](int t) { return t < 60 ? 37.5f : 37.594f; }, tau150);   // 0.006 under the threshold: not over
    assert(q.firstOn < 0);
  }

  std::printf("vent-line: OK\n");
  return 0;
}
