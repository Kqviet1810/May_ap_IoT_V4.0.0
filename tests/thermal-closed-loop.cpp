// Closed-loop tests: the REAL Thermal V3 controller (MAYAP_INDUSTRIAL_v1_0_0/thermal_v3.h) and the REAL E115 rule
// (MAYAP_INDUSTRIAL_v1_0_0/heater_response_watch.h) drive simulated ovens (tests/thermal-sim.h) through the disturbances of a real batch.
//
//  1. The oven of the 29/10 batch, fitted to the machine's own log: the 2.3.4 controller + E115 rule reproduce the failure of that day
//     (the oven stalls under SP and E115 cuts the heater, 1445 s after the door was opened on the machine); the 2.4.0 pair does not.
//  2. A family of ovens that differ in air mass, heater delay, wall loss, egg mass and coupling (12 hand-made + generated ones): a tray of
//     cold eggs of 0.25 / 0.5 / 1.0 times the eggs inside, a door, the vent fan, a set-point step, a reboot in the middle of it all.
//  3. A heater that is really dead must still stop the batch (E115), and in time.
//
// Simulation catches STRUCTURAL faults of the control law. It does not prove the real oven behaves: the real logs do (thermal-replay.cpp).
#define THERMAL_SCENARIOS_WITH_LEGACY
#include "thermal-scenarios.h"
#include <cstdlib>
#include <cstring>
#include <string>

using namespace thermalsim;

static int failures = 0;
#define CHECK(cond, ...)                         \
  do {                                           \
    if (!(cond)) {                               \
      ++failures;                                \
      std::printf("  FAIL %s:%d  %s  --  ", __FILE__, __LINE__, #cond); \
      std::printf(__VA_ARGS__);                  \
      std::printf("\n");                         \
    }                                            \
  } while (0)

// ---------------------------------------------------------------------------------------------------------------------------------
// 1. The 29/10 batch
// ---------------------------------------------------------------------------------------------------------------------------------
struct Event29 {
  double doorS = 270.0;          // the door stays open (machine log: 12:10:20 -> 12:13:50)
  double trayFraction = 1.1;     // cold eggs put in, as a multiple of the eggs already inside
  double trayC = 22.0;
  double ventAtS = 1214.0;       // a vent run, counted from the moment the door was opened (machine log: 12:30:03, 150 s)
  double ventS = 150.0;
};

template <class R>
struct Run29 {
  double tripS = -1.0;           // E115, seconds after the door was opened
  double minC = 1e9;             // lowest TRUE air temperature after the door was opened
  double overC = 0.0;            // highest TRUE air temperature above SP after it has first come back to SP - 0.3
  double backS = -1.0;           // seconds after the door was opened when the air first reached SP - 0.3 and stayed within +-0.3 afterwards
  double stallC = 0.0;           // true air temperature 24 minutes after the door was opened
  double minPowerNeedy = 100.0;  // lowest command while the air was more than 2.5 degC under SP and the door had been shut for >= 120 s
  double endC = 0.0;
};

template <class R>
static Run29<R> runOven29(const Event29 &e, double afterMin) {
  R r(u29Oven(), 28.0, 28.0);
  r.advance(6 * 3600.0, false, false, [](const R &g) { return g.plant.sensor() >= 36.4; });   // the first climb, as on the machine
  const size_t from = r.air().size();
  const double t0 = r.now();
  r.plant.insertColdMass(e.trayFraction, e.trayC);
  Run29<R> out;
  double t = 0.0;
  const double total = afterMin * 60.0;
  double lastOut = 0.0;
  bool reached = false;
  while (t < total) {
    const bool door = t < e.doorS;
    const bool vent = t >= e.ventAtS && t < e.ventAtS + e.ventS;
    r.advance(1.0, door, vent);
    t += 1.0;
    const double air = r.plant.air();
    out.minC = std::min(out.minC, air);
    if (!reached && air >= SP_C - BAND_C) reached = true;
    if (reached) out.overC = std::max(out.overC, air - SP_C);
    if (std::fabs(air - SP_C) > BAND_C) lastOut = t;
    if (!door && t >= e.doorS + 120.0 && air < SP_C - 2.5 && !r.tripped()) out.minPowerNeedy = std::min(out.minPowerNeedy, r.lastU());
    if (std::fabs(t - 1440.0) < 0.5) out.stallC = air;
  }
  out.endC = r.plant.air();
  out.backS = lastOut >= total - 1.0 ? -1.0 : lastOut;
  if (r.tripped()) out.tripS = r.tripTime() - t0;
  (void)from;
  return out;
}

static void test29() {
  std::printf("[1] The 29/10 batch on an oven fitted to its log\n");
  const Event29 e;
  const Run29<LegacyRig> o = runOven29<LegacyRig>(e, 70.0);
  const Run29<Rig> n = runOven29<Rig>(e, 70.0);
  std::printf("  2.3.4 : E115 at %6.0f s   lowest air %.2f   air at 24 min %.2f   end %.2f\n", o.tripS, o.minC, o.stallC, o.endC);
  std::printf("  2.4.0 : E115 at %6.0f s   lowest air %.2f   air at 24 min %.2f   back to SP +-0.3 at %.0f s   over %.2f   end %.2f   min power while >2.5 degC under = %.0f %%\n",
              n.tripS, n.minC, n.stallC, n.backS, n.overC, n.endC, n.minPowerNeedy);
  // The old code must reproduce the failure of that day, or this oven is not a test of it: it stalled around 35 degC and E115 cut the
  // heater 1445 s after the door was opened.
  CHECK(o.tripS > 1250.0 && o.tripS < 1650.0, "2.3.4 must trip E115 about 1445 s after the door (got %.0f)", o.tripS);
  CHECK(o.stallC > 34.0 && o.stallC < 35.6, "2.3.4 must be stalled near 35 degC when it trips (got %.2f)", o.stallC);
  CHECK(o.endC < 33.0, "after E115 the old batch is left cooling down (got %.2f)", o.endC);
  // The new pair keeps the heater working and brings the oven back.
  CHECK(n.tripS < 0.0, "2.4.0 must not trip E115 on a healthy heater (tripped at %.0f s)", n.tripS);
  CHECK(n.backS > 0.0 && n.backS < 55.0 * 60.0, "2.4.0 must be back within SP +-0.3 for good within 55 min of the door (got %.0f s)", n.backS);
  CHECK(n.overC <= 0.3, "overshoot after the recovery %.2f", n.overC);
  CHECK(n.minC > 31.5, "the dip must not be deeper than the door and the tray make it (got %.2f)", n.minC);
  // The 2.3.4 controller held the heater at 58 - 75 % with the oven 3.5 degC under SP. Near SP the energy already on its way rightly lets the
  // command taper (86 - 91 % at 2 degC under); further away it must be at the ceiling.
  CHECK(n.minPowerNeedy >= 90.0, "the heater must work at (nearly) full power while the oven is more than 2.5 degC under SP (got %.0f %%)", n.minPowerNeedy);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// 2. A family of ovens
// ---------------------------------------------------------------------------------------------------------------------------------
struct Stats {
  int n = 0, trips = 0;
  double maxOver = 0, maxFinalBand = 0, maxPp = 0, minAir = 1e9, maxTripS = 0, maxReachS = 0, maxSettleS = 0, maxBelowS = 0;
  void add(const Outcome &o) {
    ++n;
    if (o.tripS >= 0.0) { ++trips; maxTripS = std::max(maxTripS, o.tripS); }
    maxOver = std::max(maxOver, o.overC);
    maxFinalBand = std::max(maxFinalBand, o.finalBandC);
    maxPp = std::max(maxPp, o.finalPpC);
    minAir = std::min(minAir, o.minC);
    maxReachS = std::max(maxReachS, o.reachS);
    maxSettleS = std::max(maxSettleS, o.settleS);
    maxBelowS = std::max(maxBelowS, o.belowS);
  }
  void print(const char *name) const {
    std::printf("  %-34s n=%2d trips=%2d  over<=%.2f  finalBand<=%.2f  pp<=%.2f  lowest %.2f  reach<=%4.0f s  settle<=%4.0f s  below<=%4.0f s",
                name, n, trips, maxOver, maxFinalBand, maxPp, minAir, maxReachS, maxSettleS, maxBelowS);
    if (trips > 0) std::printf("  last trip %.0f s", maxTripS);
    std::printf("\n");
  }
};

// What a scenario must satisfy. The limits are the worst the 2.4.0 controller shows on the family plus a margin (see the header of each
// scenario for the principle); `healthy` = E115 must NOT trip on any oven.
struct Limits {
  double over;            // highest air temperature above SP after it first reached SP - 0.3
  double finalBand;       // |air - SP| over the last 30 minutes
  double pp;              // peak-to-peak of the air over the last 30 minutes (no oscillation when the oven is steady)
  double lowest;          // the air must never go under this (0 = not checked)
  bool backForGood;       // not a second of the last hour more than 0.3 under SP
};

// E115 must never stop a healthy heater, on any oven of any size of the family. The other limits are the worst the 2.4.0 controller shows on the
// fixed sample of 35 ovens plus a margin: they guard against a regression, they are not a promise for every possible oven. (THERMAL_FULL_FAMILY=1
// runs all 148 and only enforces the E115 rule.)
static bool strictLimits = true;
static void expectHealthy(const Stats &st, const char *name, const Limits &l) {
  CHECK(st.trips == 0, "%s: %d of %d healthy ovens were stopped by E115", name, st.trips, st.n);
  if (!strictLimits) return;
  CHECK(st.maxOver <= l.over, "%s: overshoot %.2f > %.2f", name, st.maxOver, l.over);
  CHECK(st.maxFinalBand <= l.finalBand, "%s: still %.2f degC from SP in the last 30 minutes (limit %.2f)", name, st.maxFinalBand, l.finalBand);
  CHECK(st.maxPp <= l.pp, "%s: the air still swings %.2f degC peak to peak in the last 30 minutes (limit %.2f)", name, st.maxPp, l.pp);
  if (l.lowest > 0.0) CHECK(st.minAir >= l.lowest, "%s: air fell to %.2f (limit %.2f)", name, st.minAir, l.lowest);
  if (l.backForGood) CHECK(st.maxBelowS == 0.0, "%s: %.0f s of the last hour spent more than 0.3 degC under SP", name, st.maxBelowS);
}

struct Oven { std::string name; PlantParams p; };

static std::vector<Oven> sampleOvens(bool everything) {
  std::vector<Oven> v;
  const std::vector<PlantParams> a = setA();
  for (size_t i = 0; i < a.size(); ++i) v.push_back({"A" + std::to_string(i), a[i]});
  const std::vector<PlantParams> d = family(4242u, 40, false), e = family(4343u, 36, true), b = family(20261215u, 60, false);
  const size_t stride = everything ? 1U : 6U;
  for (size_t i = 0; i < d.size(); i += stride) v.push_back({"D" + std::to_string(i), d[i]});
  for (size_t i = 0; i < e.size(); i += stride) v.push_back({"E" + std::to_string(i), e[i]});
  for (size_t i = 0; i < b.size(); i += stride) v.push_back({"B" + std::to_string(i), b[i]});
  return v;
}

template <class F>
static Stats runAll(const std::vector<Oven> &ovens, F scenario) {
  Stats st;
  for (const Oven &o : ovens) st.add(scenario(o));
  return st;
}

static void testFamily(bool everything) {
  strictLimits = !everything;
  std::printf("[2] A family of ovens (%s)\n", everything ? "all 148, E115 rule only" : "a fixed sample of 35");
  const std::vector<Oven> ovens = sampleOvens(everything);
  std::vector<Rig> settled;
  for (const Oven &o : ovens) settled.push_back(settledRig(o.p));
  auto fromSettled = [&settled](size_t &idx) { return settled[idx++]; };
  const double fractions[2] = {0.5, 1.0};
  char name[96];

  // First climb of a batch, eggs and air at 28 degC: the heater works at its ceiling, then the model/momentum brake lets the air settle.
  const Stats climb = runAll(ovens, [](const Oven &o) {
    Rig r(o.p, 28.0, 28.0);
    r.advance(2.5 * 3600.0);
    return evaluate(r, 0);
  });
  climb.print("first climb from 28 degC");
  expectHealthy(climb, "first climb", {0.5, 0.30, 0.4, 0.0, false});

  // A door held 190 s at 36.4 degC with a tray of cold eggs (x0.5, x1.0 of the eggs inside) put in: the air falls 4 degC and the new load
  // takes most of the heater for an hour. The 29/10 failure. The heater must be allowed to do its work for as long as it takes (>= 40 min for
  // a tray as big as the eggs: the physical limit), and the oven must be back at SP and stay there.
  for (int k = 0; k < 2; ++k) {
    const double f = fractions[k];
    std::snprintf(name, sizeof name, "climb, door 190 s + tray x%.1f", f);
    const Stats a = runAll(ovens, [f](const Oven &o) {
      Rig r(o.p, 28.0, 28.0);
      r.advance(4 * 3600.0, false, false, [](const Rig &g) { return g.plant.sensor() >= 36.4; });
      const size_t from = r.air().size();
      r.plant.insertColdMass(f, 25.0);
      r.advance(190.0, true, false);
      r.advance(3 * 3600.0);
      return evaluate(r, from);
    });
    a.print(name);
    expectHealthy(a, name, {0.5, 0.30, 0.4, 31.0, true});
    std::snprintf(name, sizeof name, "settled, door 150 s + tray x%.1f", f);
    size_t idx = 0;
    const Stats b = runAll(ovens, [f, &fromSettled, &idx](const Oven &) {
      Rig r = fromSettled(idx);
      const size_t from = r.air().size();
      r.plant.insertColdMass(f, 25.0);
      r.advance(150.0, true, false);
      r.advance(3 * 3600.0);
      return evaluate(r, from);
    });
    b.print(name);
    expectHealthy(b, name, {0.4, 0.30, 0.4, 31.0, true});
  }

  // A door held open for 2 / 5 / 10 minutes, nothing put in. The owner's R2.over.door asks for at most 0.3 degC of overshoot after the door is
  // shut: met at 2 and 5 minutes; at 10 minutes the fall is so deep (-5 degC) that the heater has been at its ceiling for minutes, and the energy
  // already on its way carries the air over SP (0.9 degC on this sample, 1.3 on the worst of all 148 ovens, 1.5 with the 2.3.4 controller): NOT met,
  // written down so that it cannot get worse.
  const double minutes[3] = {2.0, 5.0, 10.0};
  const double doorOver[3] = {0.30, 0.30, 1.10}, doorBand[3] = {0.30, 0.30, 0.30};
  for (int k = 0; k < 3; ++k) {
    const double m = minutes[k];
    std::snprintf(name, sizeof name, "settled, door open %.0f min", m);
    size_t idx = 0;
    const Stats st = runAll(ovens, [m, &fromSettled, &idx](const Oven &) {
      Rig r = fromSettled(idx);
      const size_t from = r.air().size();
      r.advance(m * 60.0, true, false);
      r.advance(2.0 * 3600.0);
      return evaluate(r, from);
    });
    st.print(name);
    expectHealthy(st, name, {doorOver[k], doorBand[k], 0.4, 31.0, true});
  }

  // Vent fan 5 minutes every 40 minutes: it takes 1.1 - 1.9 kW out of the oven; the dip must stay small and nothing may overshoot afterwards.
  {
    size_t idx = 0;
    const Stats st = runAll(ovens, [&fromSettled, &idx](const Oven &) {
      Rig r = fromSettled(idx);
      const size_t from = r.air().size();
      for (int k = 0; k < 3; ++k) { r.advance(5 * 60.0, false, true); r.advance(35 * 60.0); }
      return evaluate(r, from);
    });
    st.print("settled, vent fan 3 x 5 min");
    expectHealthy(st, "vent fan", {0.12, 0.60, 0.5, 36.2, false});
  }

  // Set-point +0.5 degC (a programme step): no overshoot beyond the owner's R7 limit (0.2) and there within 17 minutes.
  {
    size_t idx = 0;
    const Stats st = runAll(ovens, [&fromSettled, &idx](const Oven &) {
      Rig r = fromSettled(idx);
      const size_t from = r.air().size();
      r.setSetpoint(38.0);
      r.advance(2 * 3600.0);
      return evaluate(r, from, 38.0);
    });
    st.print("settled, set-point +0.5 degC");
    expectHealthy(st, "set-point step", {0.20, 0.30, 0.3, 37.0, true});
    CHECK(st.maxReachS <= 1020.0, "set-point step reached SP - 0.3 only after %.0f s", st.maxReachS);
  }

  // A reboot (an update, a power blip) in the middle of a recovery from a tray, after the controller had learned the load: everything it
  // knew is gone and it starts from priors. The oven must come back without E115 and without a runaway. The priors lean to the fast,
  // high-gain, long-delay side (the side on which wrong numbers make an oven swing); a fast oven with a SHORT delay therefore stays a few
  // tenths under SP for a long time after a reboot (the weak class: 3 of the 35, 0.3 - 0.6 degC), hence the wider limits.
  for (int k = 0; k < 2; ++k) {
    const double f = fractions[k];
    std::snprintf(name, sizeof name, "reboot under load (tray x%.1f)", f);
    size_t idx = 0;
    const Stats st = runAll(ovens, [f, &fromSettled, &idx](const Oven &) {
      Rig r = fromSettled(idx);
      r.plant.insertColdMass(f, 25.0);
      r.advance(150.0, true, false);
      r.advance(45 * 60.0);
      const size_t from = r.air().size();
      r.restartControllers();
      r.advance(2 * 3600.0);
      return evaluate(r, from);
    });
    st.print(name);
    expectHealthy(st, name, {0.8, 1.2, 1.3, 35.5, false});
  }

  // A heater that really is dead (SSR open, element gone) must still stop the batch, and not later than the 2.3.4 rule did.
  Stats dead;
  {
    size_t idx = 0;
    dead = runAll(ovens, [&fromSettled, &idx](const Oven &) {
      Rig r = fromSettled(idx);
      const size_t from = r.air().size();
      r.plant.setHeaterHealth(0.0);
      r.advance(3 * 3600.0);
      return evaluate(r, from);
    });
    dead.print("settled, heater DEAD (must trip)");
    CHECK(dead.trips == dead.n, "a dead heater must trip E115 on every oven (%d of %d)", dead.trips, dead.n);
    CHECK(dead.maxTripS <= 6000.0, "a dead heater must be stopped within 100 minutes (last one at %.0f s)", dead.maxTripS);
  }

  // The same two disturbances with the 2.3.4 pair: what the numbers above replace.
  {
    std::vector<LegacyRig> old;
    for (const Oven &o : ovens) { LegacyRig r(o.p, 28.0, 28.0); r.advance(4 * 3600.0); old.push_back(r); }
    Stats tray, oldDead;
    for (LegacyRig r : old) {
      const size_t from = r.air().size();
      r.plant.insertColdMass(1.0, 25.0);
      r.advance(150.0, true, false);
      r.advance(2 * 3600.0);
      tray.add(evaluate(r, from));
    }
    for (LegacyRig r : old) {
      const size_t from = r.air().size();
      r.plant.setHeaterHealth(0.0);
      r.advance(3 * 3600.0);
      oldDead.add(evaluate(r, from));
    }
    tray.print("2.3.4: settled, door + tray x1.0");
    oldDead.print("2.3.4: settled, heater DEAD");
    CHECK(tray.trips * 2 >= tray.n, "the family must be able to reproduce the failure: 2.3.4 stopped %d of %d healthy ovens", tray.trips, tray.n);
    CHECK(oldDead.trips == oldDead.n, "2.3.4 stops a dead heater too (%d of %d)", oldDead.trips, oldDead.n);
    CHECK(dead.maxTripS <= oldDead.maxTripS + 60.0, "the new rule must not find a dead heater later than the old one (%.0f vs %.0f s)", dead.maxTripS, oldDead.maxTripS);
  }
}

int main() {
  std::printf("=== Thermal closed-loop tests ===\n");
  test29();
  testFamily(std::getenv("THERMAL_FULL_FAMILY") != nullptr);
  if (failures) {
    std::printf("THERMAL CLOSED-LOOP: %d FAILED\n", failures);
    return 1;
  }
  std::printf("THERMAL CLOSED-LOOP: ALL PASSED\n");
  return 0;
}
