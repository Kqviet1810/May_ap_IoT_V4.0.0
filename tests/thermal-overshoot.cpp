// Unit tests of the two rules of 2.5.7 (MAYAP_INDUSTRIAL_v1_0_0/thermal_v3.h) against the overshoot of the restart after a protection cut the heater.
// The field log of 31/10 (tests/fixtures: the owner's lines SV 37.5 / High 37.7): the oven was never measured (priors: hold 18 %), its real hold in a hot
// afternoon is a few percent, the reading went 0.2 degC over SV, High cut the heater for 14 minutes with the reading hardly falling, and the heater
// came back with the same 17.5 % - the cause of the next overshoot. Held off by a protection, the reading is a measurement of the oven.
#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_v3.h"
#include <cassert>
#include <cmath>
#include <cstdio>

using namespace thermalv3;

static Input base(float pv) {
  Input in;
  in.sp = 37.5f; in.pv = pv; in.dQ = 0.1f; in.capPct = 100.0f; in.dtS = 0.1f; in.permit = true; in.newSample = true;
  return in;
}
// A controller that knows nothing starts by the distance to SV (2.5.8); these tests are about the hold level of 2.5.7, so they start from the prior of 18 %.
static void prior(Controller &c) { c.importHold(18.0f, false); }
static void run(Controller &c, Input in, float seconds) {
  for (int i = 0; i < static_cast<int>(seconds * 10.0f); ++i) c.step(in);
}

int main() {
  // 1. A controller that drove the reading 0.2 degC over SV and then loses its permit keeps 60 % of its hold level (once), whatever else happens.
  {
    Controller c; prior(c);
    run(c, base(37.45f), 300.0f);                                // running next to SV with the prior hold level
    const float before = c.pHold();
    assert(before > 5.0f);
    Input held = base(37.7f); held.permit = false;               // High: the permit is gone, the reading is 0.2 over
    c.step(held);
    assert(std::fabs(c.pHold() - before * V3X_F4_KEEP) < 0.05f * before);
    const float once = c.pHold();
    run(c, held, 60.0f);                                         // staying cut does not cut it again
    assert(c.pHold() > once * 0.97f);
    std::printf("overshoot then cut: hold %.1f -> %.1f %%\n", before, once);
  }
  // 2. Held off, the reading standing at/over SV and not falling: the hold level fades (the oven loses almost nothing), 14 minutes take it to ~0.
  {
    Controller c; prior(c);
    run(c, base(37.45f), 300.0f);
    const float before = c.pHold();
    Input held = base(37.6f); held.permit = false;
    run(c, held, 14.0f * 60.0f);
    assert(c.pHold() < 0.05f * before);
    // ... and the restart is not the old 17.5 %: next to SV it asks for next to nothing and lets the reading come down
    Input back = base(37.55f);
    float u = 0.0f;
    for (int i = 0; i < 600; ++i) u = c.step(back).powerPct;
    assert(u < 6.0f);
    std::printf("held off 14 min at 37.6: hold %.1f -> %.2f %%, restart asks %.1f %%\n", before, c.pHold(), u);
  }
  // 3. Held off, the reading FALLING (the oven needs its heat): the hold level is not faded.
  {
    Controller c; prior(c);
    run(c, base(37.45f), 300.0f);
    Input held = base(37.45f); held.permit = false;
    const float before = c.pHold();
    for (int s = 0; s < 15 * 60; ++s) { held.pv = 37.45f - 0.0012f * static_cast<float>(s); for (int k = 0; k < 10; ++k) c.step(held); }
    assert(c.pHold() >= before * 0.99f);
    std::printf("held off while falling 37.45 -> 36.6: hold stays %.1f %%\n", c.pHold());
  }
  // 4. A cut that is not an overshoot (the reading is under SV + 0.15) does not touch the hold level at the moment of the cut.
  {
    Controller c; prior(c);
    run(c, base(37.45f), 300.0f);
    const float before = c.pHold();
    Input held = base(37.55f); held.permit = false;
    c.step(held);
    assert(std::fabs(c.pHold() - before) < 0.01f);
  }
  // 5. The vent-line hold (shadow) keeps its own rule: no cut of the hold level at the moment it starts.
  {
    Controller c; prior(c);
    run(c, base(37.45f), 300.0f);
    const float before = c.pHold();
    Input sh = base(37.7f); sh.permit = false; sh.shadow = true;
    c.step(sh);
    assert(std::fabs(c.pHold() - before) < 0.01f);
  }
  // 6. A reading that is not valid changes nothing.
  {
    Controller c; prior(c);
    run(c, base(37.45f), 300.0f);
    const float before = c.pHold();
    Input bad = base(NAN); bad.permit = false;
    run(c, bad, 600.0f);
    assert(std::fabs(c.pHold() - before) < 0.01f);
  }
  std::printf("thermal-overshoot: OK\n");
  return 0;
}
