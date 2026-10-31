// Unit tests of the vent line (MAYAP_INDUSTRIAL_v1_0_0/vent_line.h), the code MachineController runs every cycle: when the exhaust fans of the
// overheat run. The two small fans take little heat away and their contactor is rated for few operations: the line must not chatter, and must not
// start the fans for a reading that is already coming down.
#include "../MAYAP_INDUSTRIAL_v1_0_0/vent_line.h"
#include <cassert>
#include <cstdio>

static const float ON = 37.55f, OFF = 37.5f;

// Feeds one reading per second from `startMs` for `seconds`; `temp(t)` gives the reading at second t. Returns the second the line went active
// (-1 = never).
template <class T>
static int run(VentLine &v, const VentLine::Params &p, uint32_t startMs, int seconds, T temp) {
  for (int t = 0; t <= seconds; ++t) {
    if (v.update(startMs + static_cast<uint32_t>(t) * 1000UL, true, temp(t), ON, OFF, p)) return t;
  }
  return -1;
}

int main() {
  const VentLine::Params p;

  // 1. A reading that stands one step over the threshold and does not fall: the fans start after the trend time (60 s), not before.
  {
    VentLine v;
    const int at = run(v, p, 1000U, 600, [](int) { return 37.6f; });
    assert(at >= 59 && at <= 61);
    std::printf("steady 37.6: fans start at %d s\n", at);
  }
  // 2. A reading over the threshold that is coming down (0.1 degC in 90 s) never starts them: a fall of 0.05 restarts the wait.
  {
    VentLine v;
    const int at = run(v, p, 0U, 900, [](int t) { return t < 90 ? 37.62f - 0.1f * static_cast<float>(t) / 90.0f : 37.52f; });
    assert(at < 0);
    std::printf("falling reading: the fans do not start\n");
    // ... while a reading that creeps down 0.01 degC in a minute is, for a sensor of 0.1 degC steps, not falling: the fans start.
    VentLine w;
    const int slow = run(w, p, 0U, 900, [](int t) { return 37.62f - 0.01f * static_cast<float>(t) / 60.0f; });
    assert(slow >= 59 && slow <= 61);
  }
  // 3. A decisive overshoot (a further 0.1 over the threshold) starts them after the confirm time (20 s), long before the trend time.
  {
    VentLine v;
    const int at = run(v, p, 0U, 600, [](int) { return 37.7f; });
    assert(at >= 19 && at <= 21);
    std::printf("37.7: fans start at %d s\n", at);
  }
  // 4. Once on, they run at least 3 minutes even if the reading drops under the release threshold at once, and stay off for 5 minutes.
  {
    VentLine v;
    int on = -1, off = -1, again = -1;
    for (int t = 0; t < 1500; ++t) {
      const float temp = (t < 25) ? 37.7f : (t < 100 ? 37.4f : (t < 400 ? 37.4f : 37.7f));   // over for 25 s, then cool, then over again at 400 s
      const bool a = v.update(static_cast<uint32_t>(t) * 1000UL, true, temp, ON, OFF, p);
      if (a && on < 0) on = t;
      if (!a && on >= 0 && off < 0) off = t;
      if (a && off >= 0 && again < 0) again = t;
    }
    assert(on >= 19 && on <= 21);
    assert(off - on >= 180);                     // minimum run time
    assert(off - on <= 185);
    assert(again < 0 || again - off >= 300);     // minimum rest
    std::printf("on %d s, off %d s (ran %d s), on again %d s\n", on, off, off - on, again);
  }
  // 5. At most 6 starts in an hour, however often the reading goes over: High / Emergency do not use this line and are not limited.
  {
    VentLine v;
    int starts = 0;
    bool was = false;
    for (int t = 0; t < 3600; ++t) {
      const bool over = (t % 600) < 300;         // 5 minutes over, 5 minutes under, for an hour: ten cycles
      const bool a = v.update(static_cast<uint32_t>(t) * 1000UL, true, over ? 37.8f : 37.4f, ON, OFF, p);
      if (a && !was) ++starts;
      was = a;
    }
    assert(starts <= 6 && starts >= 5);
    std::printf("ten overshoots in an hour: %d starts\n", starts);
  }
  // 6. A lost sensor value switches the line off at once and clears every wait.
  {
    VentLine v;
    run(v, p, 0U, 100, [](int) { return 37.8f; });
    assert(v.active());
    assert(!v.update(200000UL, false, 37.8f, ON, OFF, p));
    assert(!v.active());
  }
  // 7. millis() wrapping around in the middle of a wait and of a run.
  {
    VentLine v;
    const uint32_t start = UINT32_MAX - 30UL * 1000UL;
    const int at = run(v, p, start, 600, [](int) { return 37.6f; });
    assert(at >= 59 && at <= 61);
    int off = -1;
    for (int t = at; t < at + 600 && off < 0; ++t)
      if (!v.update(start + static_cast<uint32_t>(t) * 1000UL, true, 37.4f, ON, OFF, p)) off = t;
    assert(off - at >= 180 && off - at <= 200);
  }
  std::puts("VENT LINE: ALL PASSED");
  return 0;
}
