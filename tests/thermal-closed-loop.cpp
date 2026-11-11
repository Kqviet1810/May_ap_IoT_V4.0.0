// Closed-loop tests: the REAL Thermal V3 controller (MAYAP_INDUSTRIAL_v1_0_0/thermal_v3.h) and the REAL E115 rule
// (MAYAP_INDUSTRIAL_v1_0_0/heater_response_watch.h) drive simulated ovens (tests/thermal-sim.h) through the disturbances of a real batch.
//
//  1. The oven of the 29/10 batch, fitted to the machine's own log: the 2.3.4 controller + E115 rule reproduce the failure of that day
//     (the oven stalls under SP and E115 cuts the heater, 1445 s after the door was opened on the machine); the 2.4.0 pair does not.
//  2. A family of ovens that differ in air mass, heater delay, wall loss, egg mass and coupling (12 hand-made + generated ones): the first
//     climb (also of a controller that has been idle since power-up, and of an empty oven), a tray of cold eggs of 0.5 / 1.0 times the eggs
//     inside, a door, the vent fan, a set-point step, a reboot in the middle of it all, a day of ordinary operation. Every scenario also runs
//     on the 2.3.4 pair (tests/fixtures) on the same ovens and prints its numbers under the new ones: the comparison in doc/THERMAL_V3_2_8_0.md
//     is this output.
//  3. A heater that is really dead must still stop the batch (E115), and in time.
//
// Simulation catches STRUCTURAL faults of the control law. It does not prove the real oven behaves: the real logs do (thermal-replay.cpp).
#define THERMAL_SCENARIOS_WITH_LEGACY
#include "thermal-scenarios.h"
#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_hold_record.h"
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

// The oven of the 29/10 batch rebooted (an update, a power blip: the heater is off while the machine boots, 0 - 600 s) in the middle of the
// recovery from a tray as big as the eggs, once recovered, and when settled with nothing happening. Everything the controller learned is gone.
static void testReboot29() {
  const double downS[4] = {0.0, 45.0, 180.0, 600.0};
  const char *when[3] = {"25 min into the recovery", "70 min after the tray", "settled, no event"};
  for (int mode = 0; mode < 3; ++mode) {
    int trips = 0;
    double over = 0, band = 0, pp = 0;
    for (int k = 0; k < 4; ++k) {
      Rig r(u29Oven(), 28.0, 28.0);
      r.advance(6 * 3600.0, false, false, [](const Rig &g) { return g.plant.sensor() >= 36.4; });
      if (mode < 2) {
        r.plant.insertColdMass(1.0, 25.0);
        r.advance(190.0, true, false);
        r.advance((mode == 0 ? 25 : 70) * 60.0);
      } else {
        r.advance(2 * 3600.0);
      }
      const size_t from = r.air().size();
      r.restartControllers();
      r.startAt(r.now() + downS[k]);
      r.advance(2 * 3600.0);
      const Outcome o = evaluate(r, from);
      if (o.tripS >= 0.0) ++trips;
      over = std::max(over, o.overC);
      band = std::max(band, o.finalBandC);
      pp = std::max(pp, o.finalPpC);
    }
    std::printf("  reboot on the 29/10 oven, %-25s heater off 0 - 600 s: trips=%d  over<=%.2f  finalBand<=%.2f  pp<=%.2f\n", when[mode], trips, over, band, pp);
    CHECK(trips == 0, "reboot (%s): E115 stopped a healthy heater", when[mode]);
    CHECK(over <= 0.30, "reboot (%s): overshoot %.2f", when[mode], over);
    CHECK(band <= 0.20 && pp <= 0.30, "reboot (%s): %.2f degC from SP, %.2f peak to peak in the last 30 minutes", when[mode], band, pp);
  }
}

// ---------------------------------------------------------------------------------------------------------------------------------
// 2. A family of ovens
// ---------------------------------------------------------------------------------------------------------------------------------
constexpr double NEVER = 1e9;            // the settle time of an oven that does not stay within the band until the end of the run

struct Stats {
  int n = 0, trips = 0;
  double maxOver = 0, maxFinalBand = 0, maxPp = 0, minAir = 1e9, maxTripS = 0, maxReachS = 0, maxBelowS = 0;
  std::vector<double> settleS;           // per oven; an oven that never stays within the band counts as NEVER
  int farOvens = 0, farBelow90 = 0;      // ovens that fell more than 2.5 degC under SP / of them, those whose command was under 90 % at some moment
  double minFarPct = 101.0;
  bool hasR1 = false;
  double maxR1OverrunS = -1e9;           // first climbs: reach time minus the owner's R1 allowance (1.3 x the physical limit + 120 s + 720 s)
  void add(const Outcome &o) {
    ++n;
    if (o.tripS >= 0.0) { ++trips; maxTripS = std::max(maxTripS, o.tripS); }
    maxOver = std::max(maxOver, o.overC);
    maxFinalBand = std::max(maxFinalBand, o.finalBandC);
    maxPp = std::max(maxPp, o.finalPpC);
    minAir = std::min(minAir, o.minC);
    maxReachS = std::max(maxReachS, o.reachS);
    if (o.minFarPct <= 100.0) { ++farOvens; if (o.minFarPct < 90.0) ++farBelow90; minFarPct = std::min(minFarPct, o.minFarPct); }
    settleS.push_back(o.settleS < 0.0 ? NEVER : o.settleS);
    maxBelowS = std::max(maxBelowS, o.belowS);
  }
  void addR1(double overrunS) { hasR1 = true; maxR1OverrunS = std::max(maxR1OverrunS, overrunS); }
  double settlePercent(int percent) const {
    std::vector<double> v = settleS;
    std::sort(v.begin(), v.end());
    return v.empty() ? 0.0 : v[std::min(v.size() - 1, v.size() * static_cast<size_t>(percent) / 100U)];
  }
  void print(const char *name) const {
    char settle[3][16];
    for (int k = 0; k < 3; ++k) {
      const double v = settlePercent(k == 0 ? 50 : (k == 1 ? 90 : 100));
      if (v >= NEVER) std::snprintf(settle[k], sizeof settle[k], "never"); else std::snprintf(settle[k], sizeof settle[k], "%.0f", v);
    }
    std::printf("  %-40s n=%3d trips=%3d  over<=%.2f  finalBand<=%.2f  pp<=%.2f  lowest %.2f  reach<=%4.0f s  settle med/p90/max %s/%s/%s s  below<=%4.0f s",
                name, n, trips, maxOver, maxFinalBand, maxPp, minAir, maxReachS, settle[0], settle[1], settle[2], maxBelowS);
    if (farOvens > 0) std::printf("  >2.5 degC under SP in %d ovens: command under 90 %% in %d (lowest %.0f %%)", farOvens, farBelow90, minFarPct);
    if (hasR1) std::printf("  R1 overrun<=%+.0f s", maxR1OverrunS);
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
  if (l.backForGood) CHECK(st.maxBelowS == 0.0, "%s: %.0f s of the last hour spent more than 0.3 under SP", name, st.maxBelowS);
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

// Every disturbance of the family is written once and runs on both pairs (the 2.4.0 controller with the new E115 rule, and the 2.3.4 pair kept in
// tests/fixtures), on the same ovens: the 2.3.4 numbers printed under the new ones are measured here, not remembered.
// Advances `seconds` (door shut) and returns the lowest command seen while the air was more than 2.5 degC under SP, from 120 s after the door was
// shut (101 = the air never was that far under SP). The proportional law and the learned hold level decide how hard the heater works there.
template <class R>
static double advanceWatchingFarCommand(R &r, double seconds) {
  double lowest = 101.0, elapsed = 0.0;
  r.advance(seconds, false, false, [&lowest, &elapsed](const R &g) {
    elapsed += 1.0;
    if (elapsed >= 120.0 && g.plant.air() < SP_C - 2.5 && !g.tripped()) lowest = std::min(lowest, g.lastU());
    return false;
  });
  return lowest;
}

template <class R>
class Family {
 public:
  explicit Family(const std::vector<Oven> &ovens) : ovens_(ovens) {
    for (const Oven &o : ovens_) {
      R r(o.p, 28.0, 28.0);
      r.advance(4 * 3600.0);                               // the first climb and two hours of settling
      settled_.push_back(r);
    }
  }

  // The first climb of a batch, eggs and air at 28 degC. The controller has been running with the permit off for `idleS` seconds before the batch
  // starts (on the machine it runs from power-up, so every first start of a power cycle reaches it that way).
  Stats firstClimb(double idleS) const {
    Stats st;
    for (const Oven &o : ovens_) {
      R r(o.p, 28.0, 28.0);
      r.startAt(idleS);
      r.advance(idleS);
      const size_t from = r.air().size();
      r.advance(2.5 * 3600.0);
      const Outcome out = evaluate(r, from);
      st.add(out);
      st.addR1((out.reachS < 0.0 ? 2.5 * 3600.0 : out.reachS) - (1.3 * fastestReach(o.p, 28.0, 28.0) + 120.0 + 720.0));
    }
    return st;
  }
  // The first climb of an oven with nothing in it but the air and the carts (the egg capacity is a few percent of the full load).
  Stats firstClimbEmpty() const {
    return each([](const Oven &o, const R &) {
      PlantParams p = o.p;
      p.Ce = 2.0e5;
      R r(p, 28.0, 28.0);
      r.advance(2 * 3600.0);
      return evaluate(r, 0);
    });
  }
  // A door held 190 s at 36.4 degC on the first climb, with a tray of cold eggs (a fraction of the eggs inside) put in: the air falls 4 degC and
  // the new load takes most of the heater for an hour. The 29/10 failure.
  Stats climbDoorTray(double fraction) const {
    return each([fraction](const Oven &o, const R &) {
      R r(o.p, 28.0, 28.0);
      r.advance(4 * 3600.0, false, false, [](const R &g) { return g.plant.sensor() >= 36.4; });
      const size_t from = r.air().size();
      r.plant.insertColdMass(fraction, 25.0);
      r.advance(190.0, true, false);
      const double far = advanceWatchingFarCommand(r, 3 * 3600.0);
      Outcome out = evaluate(r, from);
      out.minFarPct = far;
      return out;
    });
  }
  // The same on an oven that has been settled for two hours.
  Stats settledDoorTray(double fraction) const {
    return each([fraction](const Oven &, const R &settled) {
      R r = settled;
      const size_t from = r.air().size();
      r.plant.insertColdMass(fraction, 25.0);
      r.advance(150.0, true, false);
      const double far = advanceWatchingFarCommand(r, 3 * 3600.0);
      Outcome out = evaluate(r, from);
      out.minFarPct = far;
      return out;
    });
  }
  // A door held open, nothing put in.
  Stats doorOpen(double minutes) const {
    return each([minutes](const Oven &, const R &settled) {
      R r = settled;
      const size_t from = r.air().size();
      r.advance(minutes * 60.0, true, false);
      r.advance(2.0 * 3600.0);
      return evaluate(r, from);
    });
  }
  // The controller restarted next to SP (an update, a power blip, a batch started in a warm oven) and then a door. `kept` = the machine gives it back what the
  // last climb measured (kAir and the transit time, kept in the EEPROM: thermal_gain_record.h); otherwise it has only its priors, as before 2.9.7.
  Stats restartedDoorOpen(double minutes, bool kept) const {
    return each([minutes, kept](const Oven &, const R &settled) {
      R r = settled;
      const float kAir = r.ctl.kAir(), tau = r.ctl.tauTransit();
      r.restartControllers();
      if (kept) r.ctl.restore(kAir, tau);
      r.advance(10 * 60.0);
      const size_t from = r.air().size();
      r.advance(minutes * 60.0, true, false);
      r.advance(2.0 * 3600.0);
      return evaluate(r, from);
    });
  }
  // The controller restarted in a settled oven (an update, a power blip: the machine is off for 45 s, the heater with it). With the load ledger the controller starts
  // from the level the oven needed 30 minutes ago; without it, from 12 %. `driftC` = the room changed by this much over the 40 minutes before (the load moved with it).
  Stats restartSettled(double driftC) const {
    return each([driftC](const Oven &o, const R &settled) {
      R r = settled;
      if (driftC != 0.0) {
        for (int m = 0; m < 40; ++m) { r.plant.setAmbient(o.p.Tamb + driftC * (m + 1) / 40.0); r.advance(60.0); }
      }
      const size_t from = r.air().size();
      const float kAir = r.ctl.kAir(), tau = r.ctl.tauTransit();
      r.restartControllers();
      r.ctl.restore(kAir, tau);                            // what the machine gives back from the EEPROM since 2.9.7 (thermal_gain_record.h)
      r.skipClock(45.0);
      r.startAt(r.now() + 45.0);
      r.advance(2 * 3600.0);
      return evaluate(r, from);
    });
  }
  // Vent fan 5 minutes every 40 minutes: it takes 1.1 - 1.9 kW out of the oven.
  Stats ventFan() const {
    return each([](const Oven &, const R &settled) {
      R r = settled;
      const size_t from = r.air().size();
      for (int k = 0; k < 3; ++k) { r.advance(5 * 60.0, false, true); r.advance(35 * 60.0); }
      return evaluate(r, from);
    });
  }
  // Set-point +0.5 degC (a programme step).
  Stats setpointStep() const {
    return each([](const Oven &, const R &settled) {
      R r = settled;
      const size_t from = r.air().size();
      r.setSetpoint(38.0);
      r.advance(2 * 3600.0);
      return evaluate(r, from, 38.0);
    });
  }
  // A reboot (an update, a power blip) in the middle of a recovery from a tray, after the controller had learned the load: everything it knew
  // is gone and it starts from priors. While the machine boots (45 s here) the heater is off and the controller runs idle, so it goes live with
  // an empty ring.
  Stats reboot(double fraction) const {
    return each([fraction](const Oven &, const R &settled) {
      R r = settled;
      r.plant.insertColdMass(fraction, 25.0);
      r.advance(150.0, true, false);
      r.advance(45 * 60.0);
      const size_t from = r.air().size();
      r.restartControllers();
      r.startAt(r.now() + 45.0);
      r.advance(2 * 3600.0);
      return evaluate(r, from);
    });
  }
  // A heater that really is dead (SSR open, element gone).
  Stats deadHeater() const {
    return each([](const Oven &, const R &settled) {
      R r = settled;
      const size_t from = r.air().size();
      r.plant.setHeaterHealth(0.0);
      r.advance(6 * 3600.0);                 // E115 of 3.0.1 reports after two hours at the ceiling and a fall of 1 degC: a dead heater in the best insulated oven takes up to 4.5 hours
      return evaluate(r, from);
    });
  }

 private:
  template <class F>
  Stats each(F scenario) const {
    Stats st;
    for (size_t i = 0; i < ovens_.size(); ++i) st.add(scenario(ovens_[i], settled_[i]));
    return st;
  }
  const std::vector<Oven> &ovens_;
  std::vector<R> settled_;
};

// A day of ordinary operation: a vent run of 5 minutes every 40 minutes, a door opened for 2 minutes every 5 hours, a programme step down and up
// again. Nothing here is a fault: E115 must stay far from its limit all day (the evidence is the seconds of full heater power counted without
// 2.9.9, a hot noon: the room around a settled oven warms up (5 degC in two hours) and the hold level the oven needs falls toward nothing. The
// zero zone and the dead zone must keep the air from drifting above SP by the step of the sensor; undershoot is limited too (the heater is not
// to be switched off for good at 3 % of a hold that is still there).
template <class R>
static void hotNoon(const std::vector<Oven> &ovens, double rampC, double &worstOver, double &worstUnder) {
  worstOver = worstUnder = 0.0;
  for (const Oven &o : ovens) {
    R r(o.p, 28.0, 28.0);
    r.advance(6 * 3600.0, false, false, [](const R &g) { return g.plant.sensor() >= 36.4; });
    r.advance(2 * 3600.0);
    const double sp = SP_C;
    for (double t = 0; t < 7200.0; t += 10.0) {
      r.plant.setAmbient(o.p.Tamb + rampC * t / 7200.0);
      r.advance(10.0, false, false);
      if (t > 1800.0) { worstOver = std::max(worstOver, r.plant.air() - sp); worstUnder = std::max(worstUnder, sp - r.plant.air()); }
    }
  }
}

// an answer of the temperature).
struct Soak {
  int n = 0, trips = 0;
  double peakEvidenceS = 0, worstOver = 0;      // the highest air temperature above SP (outside the door minutes)
};

template <class R>
static Soak soak(const std::vector<Oven> &ovens, double hours) {
  Soak out;
  for (const Oven &o : ovens) {
    R r(o.p, 28.0, 28.0);
    double over = 0;
    for (double t = 0; t < hours * 3600.0; t += 10.0) {
      const bool vent = std::fmod(t, 2400.0) < 300.0 && t > 4000.0;
      const bool door = std::fmod(t, 18000.0) < 120.0 && t > 14000.0;
      if (std::fabs(t - 20000.0) < 5.0) r.setSetpoint(37.0);
      if (std::fabs(t - 24000.0) < 5.0) r.setSetpoint(37.5);
      r.advance(10.0, door, vent);
      out.peakEvidenceS = std::max(out.peakEvidenceS, static_cast<double>(r.watch.accumOnMs()) / 1000.0);
      if (t > 7200.0 && !door) over = std::max(over, r.plant.air() - r.setpoint());
    }
    ++out.n;
    if (r.tripped()) ++out.trips;
    out.worstOver = std::max(out.worstOver, over);
  }
  return out;
}

static void printSoak(const char *name, const Soak &s) {
  std::printf("  %-40s n=%3d trips=%3d  over<=%.2f  E115 evidence peak %.0f s of %.0f s\n", name, s.n, s.trips, s.worstOver, s.peakEvidenceS,
              E115_ON_MS / 1000.0);
}

static void reference(const char *name, const Stats &st) {
  char line[112];
  std::snprintf(line, sizeof line, "2.3.4: %s", name);
  st.print(line);
}

// Pins of the cold-room block: measured on the fixed sample of 35 ovens, plus a margin (set from the first run, see the numbers printed by the test).
static const size_t COLD_CAPABLE_MIN = 25;                           // of the 35 (the others have a heater that cannot take the oven to SP at 10 degC)
static const int COLD_CLIMB_TRIPS_MAX = 0;                           // E115 of 3.0.1 (two hours at the ceiling and a fall) stops no oven on a first climb from 20 degC at 10 degC (3.0.0: 4 of 35, the 900 s of on-time without a 0.3 degC rise)
static const double COLD_DOOR3_OVER = 1.10, COLD_DOOR5_OVER = 1.10, COLD_TRAY_OVER = 0.50, COLD_RESTART_OVER = 0.45, COLD_BAND = 0.40, COLD_PP = 0.30, COLD_UNUSED = 0.0;
static const int NEWBATCH_TRIPS_MAX[4] = {0, 0, 0, 0};   // measured 0/0/0/0 of 21 ovens (3.0.0: 1/1/0/0: the weakest oven on a 10 degC day needed more than 900 s of on-time for a 0.3 degC rise)
static const double NEWBATCH_OVER_MAX[4] = {0.30, 0.30, 0.35, 0.50};   // 3.0.2 measured 0.19 / 0.20 / 0.23 / 0.38 of 25 ovens (the restart of a new batch no longer measures the transit time on a recovery); 3.0.1 pins: 0.75 / 0.55 / 0.35 / 0.80; //   // measured 0.66 / 0.48 / 0.29 / 0.75 of 26 ovens (3.0.0: 0.67 / 0.81 / 0.48 / 0.43 of the 21 that could settle; the other five now settle too)

static void testFamily(bool everything) {
  strictLimits = !everything;
  std::printf("[2] A family of ovens (%s); the 2.3.4 pair under each row, same ovens\n", everything ? "all 148, E115 rule only" : "a fixed sample of 35");
  const std::vector<Oven> ovens = sampleOvens(everything);
  const Family<Rig> now(ovens);
  const Family<LegacyRig> before(ovens);
  const double fractions[2] = {0.5, 1.0};
  char name[96];

  // First climb of a batch, eggs and air at 28 degC: the heater works at its ceiling, then the model/momentum brake lets the air settle.
  const Stats climb = now.firstClimb(0.0);
  climb.print("first climb from 28 degC");
  reference("first climb from 28 degC", before.firstClimb(0.0));
  expectHealthy(climb, "first climb", {0.5, 0.30, 0.4, 0.0, false});

  // The empty oven: the same controller on an oven with next to no eggs in it. The physics sets the pace there; the new pair must not be slower.
  {
    const Stats st = now.firstClimbEmpty();
    const Stats old = before.firstClimbEmpty();
    st.print("first climb, empty oven");
    reference("first climb, empty oven", old);
    expectHealthy(st, "first climb, empty oven", {0.20, 0.30, 0.4, 0.0, false});   // 3.0.1: over <= 0.11 (3.0.0: 0.26) with the landing band limited to 3 kn
    // The price of that landing: the slowest empty oven reaches SP 587 s later than at 3.0.0 (2386 -> 2973 s), 10 minutes after the 2.3.4 pair; in exchange the overshoot of the
    // first climbs of the family falls to a third (over <= 0.3: 82 -> 95 % of the ovens) and a first climb in the cold no longer swings (2.41 -> 0.83 degC peak to peak).
    CHECK(st.maxReachS <= old.maxReachS + 600.0, "empty oven: reached SP only after %.0f s (2.3.4: %.0f s)", st.maxReachS, old.maxReachS);
  }

  // The same climb after the controller has been idle (permit off) for a while: on the machine the controller runs from power-up and every
  // first start of a power cycle reaches it that way, with zeros in its ring and its history and the identification of the oven happening at
  // any position of the ring. It must do what a fresh start does (the hold level used to be thrown to about nothing by the step of the power
  // inside its window, and the approach to SP was up to 250 s slower).
  {
    const double pauses[3] = {61.0, 133.0, 307.0};
    for (int k = 0; k < 3; ++k) {
      std::snprintf(name, sizeof name, "first climb after %.0f s idle", pauses[k]);
      const Stats st = now.firstClimb(pauses[k]);
      st.print(name);
      reference(name, before.firstClimb(pauses[k]));
      expectHealthy(st, name, {0.5, 0.30, 0.4, 0.0, false});
      CHECK(st.maxReachS <= climb.maxReachS + 60.0, "%s: reached SP only after %.0f s (a fresh start: %.0f s)", name, st.maxReachS, climb.maxReachS);
    }
  }

  // A door held 190 s at 36.4 degC with a tray of cold eggs (x0.5, x1.0 of the eggs inside) put in: the 29/10 failure. The heater must be
  // allowed to do its work for as long as it takes, and the oven must be back at SP and stay there (the slowest ovens of the family, small air
  // mass and a long delay, need over an hour: the far-field gain is scaled down for them, see thermal_v3.h).
  Stats trayOld[2];
  for (int k = 0; k < 2; ++k) {
    const double f = fractions[k];
    std::snprintf(name, sizeof name, "climb, door 190 s + tray x%.1f", f);
    const Stats a = now.climbDoorTray(f);
    a.print(name);
    reference(name, before.climbDoorTray(f));
    // The overshoot of a tray as heavy as ALL the eggs inside (x1.0) is not gated: the owner sees eggs change almost nothing on the real oven, and this sink is
    // far beyond that (2.8.0). Everything else - E115, the far-field command, the final band, the swing, the lowest air, back at SP - is.
    expectHealthy(a, name, {k == 1 ? 9.9 : 0.5, 0.30, 0.4, 31.0, true});
    std::snprintf(name, sizeof name, "settled, door 150 s + tray x%.1f", f);
    const Stats b = now.settledDoorTray(f);
    b.print(name);
    trayOld[k] = before.settledDoorTray(f);
    reference(name, trayOld[k]);
    expectHealthy(b, name, {0.4, 0.30, 0.4, 31.0, true});
  }

  // A door held open for 2 / 5 / 10 minutes, nothing put in. The owner's R2.over.door asks for at most 0.3 degC of overshoot after the door is
  // shut: met at 2 and 5 minutes; at 10 minutes the fall is so deep (-5 degC) that the heater has been at its ceiling for minutes, and the energy
  // already on its way carries the air over SP (0.9 degC on this sample, 1.3 on the worst of all 148 ovens): NOT met, written down so that it
  // cannot get worse.
  const double minutes[3] = {2.0, 5.0, 10.0};
  const double doorOver[3] = {0.30, 0.30, 1.10}, doorBand[3] = {0.30, 0.30, 0.30};
  for (int k = 0; k < 3; ++k) {
    std::snprintf(name, sizeof name, "settled, door open %.0f min", minutes[k]);
    const Stats st = now.doorOpen(minutes[k]);
    st.print(name);
    reference(name, before.doorOpen(minutes[k]));
    expectHealthy(st, name, {doorOver[k], doorBand[k], 0.4, 31.0, true});
  }

  // Restarted next to SP, then a door. With the numbers the last climb measured (2.9.7) every oven of the sample must come back like a settled one. Without them
  // (a machine that has never made a climb, or whose record was cleared) the controller has only its priors: three fast ovens of the sample then hunt around SP
  // and are still 0.8 degC away an hour later - printed for the record, not a limit (it is what the stored numbers are there to prevent).
  for (int k = 0; k < 3; ++k) {
    std::snprintf(name, sizeof name, "restarted next to SP, numbers kept, door open %.0f min", minutes[k]);
    const Stats st = now.restartedDoorOpen(minutes[k], true);
    st.print(name);
    expectHealthy(st, name, {doorOver[k] + 0.2, doorBand[k], 0.4, 31.0, true});
  }
  for (int k = 0; k < 3; ++k) {
    std::snprintf(name, sizeof name, "restarted next to SP, priors only, door open %.0f min", minutes[k]);
    now.restartedDoorOpen(minutes[k], false).print(name);
  }

  // Vent fan 5 minutes every 40 minutes: the dip must stay small and nothing may overshoot afterwards.
  {
    const Stats st = now.ventFan();
    st.print("settled, vent fan 3 x 5 min");
    reference("settled, vent fan 3 x 5 min", before.ventFan());
    expectHealthy(st, "vent fan", {0.12, 0.60, 0.5, 36.2, false});
  }

  // Set-point +0.5 degC (a programme step): no overshoot beyond the owner's R7 limit (0.2) and there within 17 minutes.
  {
    const Stats st = now.setpointStep();
    st.print("settled, set-point +0.5 degC");
    reference("settled, set-point +0.5 degC", before.setpointStep());
    expectHealthy(st, "set-point step", {0.20, 0.30, 0.3, 37.0, true});
    CHECK(st.maxReachS <= 1020.0, "set-point step reached SP - 0.3 only after %.0f s", st.maxReachS);
  }

  // A reboot in the middle of a recovery from a tray. The oven must come back without E115 and without a runaway. The priors lean to the fast,
  // high-gain, long-delay side (the side on which wrong numbers make an oven swing); a fast oven with a SHORT delay therefore stays a few
  // tenths under SP for a long time after a reboot (the weak class: 3 of the 35, 0.3 - 0.7 degC), hence the wider limits.
  for (int k = 0; k < 2; ++k) {
    std::snprintf(name, sizeof name, "reboot under load (tray x%.1f)", fractions[k]);
    const Stats st = now.reboot(fractions[k]);
    st.print(name);
    reference(name, before.reboot(fractions[k]));
    expectHealthy(st, name, {k == 1 ? 9.9 : 0.8, 1.2, 1.3, 35.5, false});   // x1.0: overshoot not gated, see above
  }

  // A day of ordinary operation on the hand-made ovens (all of the family when everything is asked for).
  {
    std::vector<Oven> day;
    for (const Oven &o : ovens) if (everything || o.name[0] == 'A') day.push_back(o);
    const Soak st = soak<Rig>(day, 24.0);
    printSoak("24 h of ordinary operation", st);
    printSoak("2.3.4: 24 h of ordinary operation", soak<LegacyRig>(day, 24.0));
    CHECK(st.trips == 0, "ordinary operation stopped %d of %d healthy ovens with E115", st.trips, st.n);
    CHECK(st.peakEvidenceS <= 0.65 * E115_ON_MS / 1000.0, "E115 evidence came within %.0f s of its limit of %.0f s in ordinary operation", st.peakEvidenceS, E115_ON_MS / 1000.0);
    if (strictLimits) CHECK(st.worstOver <= 0.60, "ordinary operation: the air went %.2f degC over SP", st.worstOver);
  }

  {
    double over = 0, under = 0;
    hotNoon<Rig>(ovens, 5.0, over, under);
    std::printf("  hot noon (+5 degC of room in 2 h): highest %+.2f over SP, lowest %.2f under SP\n", over, under);
    if (strictLimits) {
      CHECK(over < 0.15, "hot noon: the air rose %.2f over SP (limit 0.15)", over);
      CHECK(under < 0.10, "hot noon: the air fell %.2f under SP (limit 0.10)", under);
    }
  }

  // 2.9.10, slow batch starts: the oven fitted to the log of 05/11 14:37 (a heater that delivers a quarter of its power, UA 100, restarted 0.3 degC under SP) and
  // three more loads under SP. The request rose 0.6 %/min while PV stood flat; the stall lift (thermal_v3.h, stallLift) pays the missing rise into the hold, faster
  // in the first minutes of a start that has not reached SV yet, but only while PV is more than the gate of 0.5 degC under SV: nearer than that it made a new batch of cold eggs
  // overshoot (up to 1.19 degC against 0.66 of 2.9.9). 2.9.9 reached SP - 0.1 after 51 / 45 / 40 / 24 minutes; the first case (a stall only 0.3 - 0.5 degC under SV) is therefore
  // not faster than before (pinned at 55), the other three are.
  {
    struct Weak { const char *name; double health, start, limitMin, limitOver; };
    const Weak cases[4] = {{"05/11 fit (25 % heater, 37.2 start)", 0.25, 37.2, 55.0, 0.20}, {"25 % heater from 36.9", 0.25, 36.9, 32.0, 0.30},
                           {"35 % heater from 36.5", 0.35, 36.5, 26.0, 0.40}, {"50 % heater from 36.9", 0.50, 36.9, 22.0, 0.25}};
    for (const Weak &w : cases) {
      PlantParams wp = u29Oven(); wp.Pmax *= w.health; wp.UA = 100.0;
      Rig r(wp, w.start, 37.0); r.plant.settleAt(w.start);
      double reachMin = -1.0, over = 0.0;
      for (int m = 1; m <= 90; ++m) {
        r.advance(60.0);
        const double a = r.plant.air();
        if (reachMin < 0.0 && a >= SP_C - 0.1) reachMin = m;
        if (reachMin >= 0.0) over = std::max(over, a - SP_C);
      }
      std::printf("  slow start, %s: SP - 0.1 reached after %.0f min, highest %+.2f over SP, E115 %s\n", w.name, reachMin, over, r.tripped() ? "TRIPPED" : "no");
      CHECK(!r.tripped(), "slow start (%s): E115 tripped on a weak heater that was still climbing", w.name);
      CHECK(reachMin > 0.0 && reachMin <= w.limitMin, "slow start (%s): SP - 0.1 reached after %.0f min (limit %.0f)", w.name, reachMin, w.limitMin);
      CHECK(over < w.limitOver, "slow start (%s): the air rose %.2f over SP (limit %.2f)", w.name, over, w.limitOver);
    }
  }

  // 2.9.10, a cold room: the worst winter day, 10 degC around an oven that is built for 28 (the heat the oven loses goes up 2.9 times, so does the hold level it needs).
  // Ovens of the sample whose heater cannot take the oven to SP at 10 degC are not the controller's fault and are left out (their number is printed and pinned).
  // Then the new-batch start from the controller's own prior (the room of the new batch can be the same, colder or warmer than the room of the last one).
  {
    std::vector<Oven> coldAll, warmAll;
    for (const Oven &o : ovens) { Oven c = o; c.p.Tamb = 10.0; coldAll.push_back(c); warmAll.push_back(o); }
    std::vector<size_t> idx;                                                  // ovens that can hold SP at 10 degC
    std::vector<Rig> coldSettled, warmSettled;
    Stats climbCold;
    for (size_t i = 0; i < coldAll.size(); ++i) {
      Rig r(coldAll[i].p, 20.0, 20.0);                                       // eggs and air from a cool room
      r.advance(4 * 3600.0);
      const Outcome out = evaluate(r, 0);
      climbCold.add(out);
      if (out.reachS >= 0.0 && out.tripS < 0.0 && out.finalBandC <= 0.30) {
        idx.push_back(i); coldSettled.push_back(r);
        Rig w(warmAll[i].p, 28.0, 28.0); w.advance(4 * 3600.0); warmSettled.push_back(w);
      }
    }
    climbCold.print("cold room 10 degC: first climb from 20");
    std::printf("  cold room 10 degC: %zu of %zu ovens can hold SP\n", idx.size(), coldAll.size());
    CHECK(climbCold.trips <= COLD_CLIMB_TRIPS_MAX, "cold room: E115 stopped %d ovens on the first climb (pin %d)", climbCold.trips, COLD_CLIMB_TRIPS_MAX);
    CHECK(climbCold.maxOver <= 0.70 && climbCold.maxPp <= 1.0, "cold room: first climb over %.2f, swing %.2f degC (pins 0.70 / 1.0; 3.0.0: 2.56 / 2.41)", climbCold.maxOver, climbCold.maxPp);
    CHECK(idx.size() >= COLD_CAPABLE_MIN, "cold room: only %zu of %zu ovens can hold SP at 10 degC (pin %zu)", idx.size(), coldAll.size(), static_cast<size_t>(COLD_CAPABLE_MIN));
    auto scenario = [&](const char *name, const Limits &l, bool door, double minutes, double tray, bool restart) {
      Stats st;
      for (size_t k = 0; k < idx.size(); ++k) {
        Rig r = coldSettled[k];
        const size_t from = r.air().size();
        if (tray > 0.0) r.plant.insertColdMass(tray, 25.0);
        if (door) r.advance(minutes * 60.0, true, false);
        if (restart) { const float kAir = r.ctl.kAir(), tau = r.ctl.tauTransit(); r.restartControllers(); r.ctl.restore(kAir, tau); r.skipClock(45.0); r.startAt(r.now() + 45.0); }
        r.advance(2 * 3600.0);
        st.add(evaluate(r, from));
      }
      st.print(name);
      expectHealthy(st, name, l);
    };
    scenario("cold room: settled, door open 3 min", {COLD_DOOR3_OVER, COLD_BAND, COLD_PP, 0.0, false}, true, 3.0, 0.0, false);
    scenario("cold room: settled, door open 5 min", {COLD_DOOR5_OVER, COLD_BAND, COLD_PP, 0.0, false}, true, 5.0, 0.0, false);
    scenario("cold room: settled, door 150 s + tray x0.5", {COLD_TRAY_OVER, COLD_BAND, COLD_PP, 0.0, false}, true, 2.5, 0.5, false);
    scenario("cold room: restart in a settled oven", {COLD_RESTART_OVER, COLD_BAND, COLD_PP, 0.0, false}, false, 0.0, 0.0, true);

    // The new batch: eggs of 25 degC go in (door 150 s), the machine starts the batch at once from its own 12 % hold prior: nothing of the last batch is carried over (the hold is read
    // again by the feedback, and by the evidence of the heater-stuck rule when that is slow). `from` / `to` = the room of the last batch / of this one.
    auto newBatch = [&](const std::vector<Rig> &fromRigs, double toRoom) {
      Stats st;
      for (size_t k = 0; k < idx.size(); ++k) {
        Rig r = fromRigs[k];
        r.plant.setAmbient(toRoom);
        r.plant.insertColdMass(0.5, 25.0);
        r.advance(150.0, true, false);
        const size_t from = r.air().size();
        const float kAir = r.ctl.kAir(), tau = r.ctl.tauTransit();
        r.restartControllers();
        r.ctl.restore(kAir, tau);
        r.skipClock(45.0);
        r.startAt(r.now() + 45.0);
        r.advance(2 * 3600.0);
        st.add(evaluate(r, from));
      }
      return st;
    };
    struct NB { const char *name; const std::vector<Rig> *rigs; double room; };
    const NB nbs[4] = {{"last batch 28 degC, this one 10 (colder day)", &warmSettled, 10.0}, {"last batch 10 degC, this one 10", &coldSettled, 10.0},
                       {"last batch 10 degC, this one 28 (warmer day)", &coldSettled, 28.0}, {"last batch 28 degC, this one 28", &warmSettled, 28.0}};
    for (int k = 0; k < 4; ++k) {
      const std::string a = std::string("new batch, ") + nbs[k].name;
      const Stats st = newBatch(*nbs[k].rigs, nbs[k].room);
      st.print(a.c_str());
      CHECK(st.trips <= NEWBATCH_TRIPS_MAX[k], "%s: E115 tripped on %d ovens (pin %d)", nbs[k].name, st.trips, NEWBATCH_TRIPS_MAX[k]);
      CHECK(st.maxOver <= NEWBATCH_OVER_MAX[k], "%s: the air went %.2f over SP (pin %.2f)", nbs[k].name, st.maxOver, NEWBATCH_OVER_MAX[k]);
    }
  }

  {
    const Stats a = now.restartSettled(0.0), b = now.restartSettled(-2.0), c = now.restartSettled(2.0);
    a.print("restart in a settled oven");
    b.print("restart, room 2 degC colder over 40 min");
    c.print("restart, room 2 degC warmer over 40 min");
    expectHealthy(a, "restart in a settled oven", {0.50, 0.14, 0.30, 0.0, false});   // 3.0.2: over 0.40, band 0.12, swing 0.20
  }

  // A heater that really is dead (SSR open, element gone) must still stop the batch, and not later than the 2.3.4 rule did.
  const Stats dead = now.deadHeater();
  const Stats deadOld = before.deadHeater();
  dead.print("settled, heater DEAD (must trip)");
  reference("settled, heater DEAD (must trip)", deadOld);
  // E115 of 3.0.1 is the simple rule (the request at 85 % of the ceiling or more for two hours and the air 1 degC under its highest value of that stretch): a dead heater is reported
  // by every oven of the family, in the best insulated ones after up to 4.5 hours, in exchange for no false stop of a healthy oven under any load of this file (the first rule, 900 s
  // of on-time without a rise of 0.3 degC, stopped 5 of 21 healthy ovens under a heavy tray in the cold). 2.3.4 found it sooner and stopped healthy ovens.
  CHECK(dead.trips == dead.n, "a dead heater must trip E115 on every oven (%d of %d)", dead.trips, dead.n);
  CHECK(dead.maxTripS <= 5.0 * 3600.0, "a dead heater must be reported within 5 hours (last one at %.0f s)", dead.maxTripS);
  CHECK(deadOld.trips == deadOld.n, "2.3.4 stops a dead heater too (%d of %d)", deadOld.trips, deadOld.n);

  // The family must be able to reproduce the failure of 29/10 with the old pair, or it is not a test of the new one.
  CHECK(trayOld[1].trips * 2 >= trayOld[1].n, "the family must be able to reproduce the failure: 2.3.4 stopped %d of %d healthy ovens", trayOld[1].trips, trayOld[1].n);
}

// The day profile of the exhaust fan takes a different amount of heat out on every day of the year (a cold day: 25 points of the heater, a warm one: nothing, and in the summer
// the air it brings in can even add heat). The controller learns the compensation (its size by the mean error of each run, its timing by hill-climbing on the energy of the
// error) run after run; it must find the winter size, the autumn size and the summer size by itself. Oven of the 29/10 batch, fan 7.2 min in every 40.
static void testFanProfile() {
  std::printf("\n[F] The exhaust fan on a day profile, at every season (learned, nothing fixed)\n");
  const double PERIOD = 2400.0, ON = 432.0;
  struct Day { double tamb; double dipMax, overMax; };
  const Day days[] = {{4.0, 0.12, 0.12}, {14.0, 0.08, 0.08}, {24.0, 0.10, 0.10}, {34.0, 0.08, 0.08}};   // measured on the sixteenth run: 0.08/0.08, 0.02/0.04, 0.06/0.06, 0.00/0.03 (3.0.0: 0.25/0.20, 0.20/0.15, 0.20/0.15, 0.15/0.15; the 2.9.11 controller: 0.73 / 0.61 / 0.36 / 0.11 under SV)
  for (const Day &d : days) {
    thermalsim::PlantParams p = thermalsim::u29Oven(); p.Tamb = d.tamb;
    Rig r(p, 25.0, 25.0); r.advance(5 * 3600.0);
    const double base = r.now();
    double dipLast = 0.0, overLast = 0.0;
    for (int c = 0; c < 16; ++c) {
      double mn = 99.0, mx = -99.0, holdAtStart = 0.0, holdMoved = 0.0;
      for (int s = 0; s < static_cast<int>(PERIOD); ++s) {
        const double ph = std::fmod(r.now() - base, PERIOD);
        const bool vent = ph < ON;
        r.in.fanOnInS = vent ? 0.0f : static_cast<float>(PERIOD - ph);
        r.in.fanOffInS = vent ? static_cast<float>(ON - ph) : static_cast<float>(NAN);
        if (s == 0) holdAtStart = r.ctl.pHold();
        r.advance(1.0, false, vent);
        const double a = r.plant.air(); mn = std::min(mn, a); mx = std::max(mx, a);
        if (ph < ON + 2.0 * r.ctl.tauTransit()) holdMoved = std::max(holdMoved, std::fabs(static_cast<double>(r.ctl.pHold()) - holdAtStart));   // the run and the two transit times after it
      }
      dipLast = thermalsim::SP_C - mn; overLast = mx - thermalsim::SP_C;
      if (c == 15) CHECK(holdMoved <= 0.5, "room %.0f: the load estimate moved %.2f points inside the sixteenth fan run (it must stay where it was: the compensation's own transient is not a load)", d.tamb, holdMoved);
    }
    std::printf("  room %4.0f degC: sixteenth run: dip %.2f, rise %.2f degC (hold %.1f %%)\n", d.tamb, dipLast, overLast, static_cast<double>(r.ctl.pHold()));
    CHECK(!r.tripped(), "room %.0f: E115 must not trip on a healthy oven with the fan on its profile", d.tamb);
    CHECK(dipLast <= d.dipMax, "room %.0f: the fan still pulls the air %.2f under SV on the sixteenth run (limit %.2f)", d.tamb, dipLast, d.dipMax);
    CHECK(overLast <= d.overMax, "room %.0f: the compensation leaves the air %.2f over SV on the sixteenth run (limit %.2f)", d.tamb, overLast, d.overMax);
  }
}

// A restart next to SV in the middle of the fan profile (firmware 3.0.1): the machine restores kAir, tau, 0.9 x the hold and, now, the learned compensation of the fan (it is part of
// the hold record). Without it the first run of the fan after the restart is as bad as with no compensation at all (log of 10/11 14:30: dip 0.36 degC, request +2.5 points on the
// hold); with it the first run is compensated. Oven of the 29/10 batch, fan 7.2 min in every 40, the machine restarts 16 minutes before a run.
static void testRestartFan() {
  std::printf("\n[R] A restart in the middle of the fan profile: the compensation of the fan is restored with the hold\n");
  const double PERIOD = 2400.0, ON = 432.0;
  struct Day { double tamb, dipMax, overMax; };
  const Day days[] = {{12.0, 0.12, 0.10}, {22.0, 0.10, 0.10}, {30.0, 0.10, 0.10}};
  for (const Day &d : days) {
    double dip[2] = {0.0, 0.0}, over[2] = {0.0, 0.0};
    for (int restore = 0; restore < 2; ++restore) {
      thermalsim::PlantParams p = thermalsim::u29Oven(); p.Tamb = d.tamb;
      Rig r(p, 25.0, 25.0); r.advance(5 * 3600.0);
      const double base = r.now();
      auto runTo = [&](double tEnd) {
        while (r.now() - base < tEnd - 1e-9) {
          const double ph = std::fmod(r.now() - base, PERIOD);
          const bool vent = ph < ON;
          r.in.fanOnInS = vent ? 0.0f : static_cast<float>(PERIOD - ph);
          r.in.fanOffInS = vent ? static_cast<float>(ON - ph) : static_cast<float>(NAN);
          r.advance(1.0, false, vent);
        }
      };
      runTo(12 * PERIOD + (PERIOD - 1000.0));
      const thermalv3::Controller old = r.ctl;
      const float kA = r.ctl.kAir(), tau = r.ctl.tauTransit(), hold = r.ctl.pHold();
      r.restartControllers(); r.ctl.restore(kA, tau); r.ctl.restoreHold(0.9f * hold);
      if (restore) r.ctl.restoreFan(old.fanComp(), old.fanLeadS());
      r.skipClock(45.0);
      const double t0 = r.now() - base;
      runTo(t0 + 2 * PERIOD);
      const auto &a = r.air(); const size_t n0 = a.size() - static_cast<size_t>(2 * PERIOD);
      double mn = 99.0, mx = -99.0;
      for (size_t i = n0; i < a.size(); ++i) {
        const double sec = static_cast<double>(i - n0);
        if (sec >= 1000.0 && sec < 1000.0 + ON + 600.0) { mn = std::min(mn, static_cast<double>(a[i])); mx = std::max(mx, static_cast<double>(a[i])); }
      }
      dip[restore] = thermalsim::SP_C - mn; over[restore] = mx - thermalsim::SP_C;
      if (restore) CHECK(!r.tripped(), "room %.0f: E115 must not trip after the restart", d.tamb);
    }
    std::printf("  room %4.0f degC: first run after the restart: lost  dip %.2f rise %.2f | restored  dip %.2f rise %.2f degC\n", d.tamb, dip[0], over[0], dip[1], over[1]);
    CHECK(dip[1] <= d.dipMax, "room %.0f: the restored compensation leaves a dip of %.2f on the first run (limit %.2f)", d.tamb, dip[1], d.dipMax);
    CHECK(over[1] <= d.overMax, "room %.0f: the restored compensation leaves a rise of %.2f on the first run (limit %.2f)", d.tamb, over[1], d.overMax);
    CHECK(dip[1] <= 0.5 * dip[0], "room %.0f: restoring the compensation must at least halve the first dip (%.2f against %.2f)", d.tamb, dip[1], dip[0]);
  }
}


// A protection at SV + 0.1 (the owner's E111 margin) cuts the heater now and then. The compensation of the fan must not run away from it (3.0.0: a cut inside a run truncated the
// compensation, the run looked like a dip, the learner raised it, the next run overshot): on the sixteenth run the air is again within a few hundredths and the cuts have stopped.
static void testFanWithProtection() {
  std::printf("\n[P] The fan compensation with the protection at SV + 0.1 degC\n");
  const double PERIOD = 2400.0, ON = 432.0;
  for (double tamb : {4.0, 14.0, 24.0}) {
    thermalsim::PlantParams p = thermalsim::u29Oven(); p.Tamb = tamb;
    Rig r(p, 25.0, 25.0); r.protectOverC = 0.1; r.advance(5 * 3600.0);
    const double base = r.now(); int cutsAt11 = 0; double dipLast = 0.0, overLast = 0.0;
    for (int c = 0; c < 16; ++c) {
      double mn = 99.0, mx = -99.0;
      for (int s = 0; s < static_cast<int>(PERIOD); ++s) {
        const double ph = std::fmod(r.now() - base, PERIOD); const bool vent = ph < ON;
        r.in.fanOnInS = vent ? 0.0f : static_cast<float>(PERIOD - ph);
        r.in.fanOffInS = vent ? static_cast<float>(ON - ph) : static_cast<float>(NAN);
        r.advance(1.0, false, vent);
        mn = std::min(mn, static_cast<double>(r.plant.air())); mx = std::max(mx, static_cast<double>(r.plant.air()));
      }
      if (c == 11) cutsAt11 = r.protectionCuts();
      dipLast = thermalsim::SP_C - mn; overLast = mx - thermalsim::SP_C;
    }
    const int cutsLate = r.protectionCuts() - cutsAt11;
    std::printf("  room %4.0f degC: %d cuts in 16 runs, %d of them in the last four; sixteenth run: dip %.2f, rise %.2f degC (compensation %.1f %%)\n", tamb, r.protectionCuts(), cutsLate, dipLast, overLast, static_cast<double>(r.ctl.fanComp()));
    CHECK(!r.tripped(), "room %.0f: E115 must not trip", tamb);
    CHECK(cutsLate <= 2, "room %.0f: %d protection cuts in the last four runs (limit 2)", tamb, cutsLate);
    CHECK(dipLast <= 0.15 && overLast <= 0.15, "room %.0f: sixteenth run dip %.2f rise %.2f (limit 0.15)", tamb, dipLast, overLast);
    CHECK(r.ctl.fanComp() < 0.4f * 100.0f - 1.0f, "room %.0f: the compensation sits at its ceiling (%.1f %%)", tamb, static_cast<double>(r.ctl.fanComp()));
  }
}

// After a door and a tray of cold mass the climb back is loaded with that mass; its slope says how much the load takes, not what the heater gives. kAir (and with it every threshold) must
// stay where the cold start measured it (log of 10/11: 22e-5 -> 3e-5 -> 80e-5 on one oven in two hours), and so must the transit time.
static void testTrayKeepsTheOvenNumbers() {
  std::printf("\n[T] A tray of cold mass after a door: the numbers of the oven stay\n");
  auto fleet = thermalsim::family(4343u, 6, true); fleet.push_back(thermalsim::u29Oven());
  double kLo = 1e9, kHi = 0.0, tLo = 1e9, tHi = 0.0;
  for (thermalsim::PlantParams p : fleet) {
    p.Tamb = 20.0;
    Rig r(p, 25.0, 25.0); r.advance(5 * 3600.0);
    const double k0 = r.ctl.kAir(), t0 = r.ctl.tauTransit();
    r.advance(180.0, true, false); r.plant.insertColdMass(1.0, 12.0);
    for (int s = 0; s < 3 * 3600; ++s) {
      r.advance(1.0);
      kLo = std::min(kLo, r.ctl.kAir() / k0); kHi = std::max(kHi, r.ctl.kAir() / k0);
      tLo = std::min(tLo, r.ctl.tauTransit() / t0); tHi = std::max(tHi, r.ctl.tauTransit() / t0);
    }
  }
  std::printf("  kAir after / before: %.2f .. %.2f   transit time after / before: %.2f .. %.2f   (%zu ovens)\n", kLo, kHi, tLo, tHi, fleet.size());
  CHECK(kLo >= 0.9 && kHi <= 1.1, "kAir moved to %.2f .. %.2f of its value after a tray (limit 0.9 .. 1.1)", kLo, kHi);
  CHECK(tLo >= 0.9 && tHi <= 1.1, "the transit time moved to %.2f .. %.2f of its value after a tray (limit 0.9 .. 1.1)", tLo, tHi);
}

static const double SHARE_MIN = 0.55, TAU_RATIO_LO = 0.60, TAU_RATIO_HI = 1.35;   // measured 0.62; 0.72 .. 1.19 (3.0.1: 0.39; up to 1.75)

// What broke the machine on 11/11, as one scenario: a restart with a stored kAir twice the oven's, the fan compensation restored at its ceiling (40 points), the protection at SV + 0.1
// and the fan on its profile, room 12 degC. 3.0.1 kept less than 40 % of the time inside +-0.1 degC, asked for power above the hold while the air stood over SV and was cut 118 times
// on 17 ovens. Invariants: no heat above the hold (plus the fan compensation) while the reading is over SV; the air back inside +-0.1 degC most of the time.
static void testWrongNumbersAndProtection() {
  std::printf("\n[W] Restart with kAir x2, the compensation at its ceiling, protection at SV + 0.1, fan profile, room 12\n");
  auto fleet = thermalsim::family(4343u, 5, true); fleet.push_back(thermalsim::u29Oven());
  int n = 0, cuts = 0; double heatAbove = 0.0, share = 0.0, worstOver = 0.0;
  for (thermalsim::PlantParams p : fleet) {
    p.Tamb = 12.0;
    if ((p.UA + p.ventW / 12.5) * (thermalsim::SP_C - 2.0) / p.Pmax > 0.85) continue;
    Rig r(p, 25.0, 25.0); r.advance(5 * 3600.0);
    const float kA = r.ctl.kAir(), tau = r.ctl.tauTransit(), hold = r.ctl.pHold();
    r.restartControllers(); r.ctl.restore(2.0f * kA, tau); r.ctl.restoreHold(0.9f * hold); r.ctl.restoreFan(40.0f, tau); r.skipClock(45.0); r.startAt(r.now() + 45.0);
    r.protectOverC = 0.1;
    const double base = r.now(); long in10 = 0, cnt = 0;
    for (long s = 0; s < 6 * 3600; ++s) {
      const double ph = std::fmod(r.now() - base + 1200.0, 2400.0); const bool vent = ph < 432.0;
      r.in.fanOnInS = vent ? 0.0f : static_cast<float>(2400.0 - ph); r.in.fanOffInS = vent ? static_cast<float>(432.0 - ph) : static_cast<float>(NAN);
      r.advance(1.0, false, vent);
      const double e = thermalsim::SP_C - r.plant.air();
      if (r.in.pv >= thermalsim::SP_C + 0.05 && r.lastU() > r.ctl.pHold() + r.ctl.fanAdded() + 1.0) heatAbove += 1.0;
      worstOver = std::max(worstOver, -e);
      if (s > 1800) { ++cnt; in10 += std::fabs(e) <= 0.1; }
    }
    ++n; cuts += r.protectionCuts(); share += static_cast<double>(in10) / static_cast<double>(cnt);
    CHECK(!r.tripped(), "E115 must not trip (oven %d)", n);
  }
  share /= n;
  std::printf("  %d ovens: %.0f %% of the time inside +-0.1 degC, %d protection cuts, %.0f s of heat above the hold with the reading over SV, highest %.2f over SV\n", n, 100.0 * share, cuts, heatAbove, worstOver);
  CHECK(heatAbove == 0.0, "%.0f s of power above the hold while the reading stood over SV (the prediction of a fall must not add heat)", heatAbove);
  CHECK(share >= SHARE_MIN, "only %.0f %% of the time inside +-0.1 degC (pin %.0f %%)", 100.0 * share, 100.0 * SHARE_MIN);
}

// The transit time is the start of the oven's answer: between the dead time and dead time + lag of the plant, not the moment the rise could be trusted (3.0.1 read 201 s on an oven
// whose delay is 115 s, and every gain is derived from it). A new batch after a restart keeps the stored value: a recovery after a door is not a start from rest.
static void testTransitTime() {
  std::printf("\n[D] The transit time the controller measures against the plant's delay\n");
  auto fleet = thermalsim::family(4343u, 8, true); auto more = thermalsim::family(4242u, 8, false); fleet.insert(fleet.end(), more.begin(), more.end()); fleet.push_back(thermalsim::u29Oven());
  double worstLo = 9.0, worstHi = 0.0;
  for (thermalsim::PlantParams p : fleet) {
    p.Tamb = 20.0;
    Rig r(p, 25.0, 25.0); r.advance(4 * 3600.0);
    const double ratio = r.ctl.tauTransit() / (p.td + p.tl);
    worstLo = std::min(worstLo, ratio); worstHi = std::max(worstHi, ratio);
    // the new batch: door, cold eggs, restart with the stored numbers
    const float kA = r.ctl.kAir(), tau = r.ctl.tauTransit();
    r.plant.insertColdMass(0.5, 25.0); r.advance(150.0, true, false);
    r.restartControllers(); r.ctl.restore(kA, tau); r.skipClock(45.0); r.startAt(r.now() + 45.0);
    r.advance(3600.0);
    CHECK(std::fabs(r.ctl.tauTransit() - tau) < 0.5f, "a new batch after a restart moved the transit time %.0f -> %.0f s", static_cast<double>(tau), static_cast<double>(r.ctl.tauTransit()));
  }
  std::printf("  measured / (dead time + lag): %.2f .. %.2f over %zu ovens\n", worstLo, worstHi, fleet.size());
  CHECK(worstLo >= TAU_RATIO_LO && worstHi <= TAU_RATIO_HI, "transit time %.2f .. %.2f of the plant's (pins %.2f .. %.2f)", worstLo, worstHi, TAU_RATIO_LO, TAU_RATIO_HI);
}

int main() {
  std::printf("=== Thermal closed-loop tests ===\n");
  test29();
  testReboot29();
  testFamily(std::getenv("THERMAL_FULL_FAMILY") != nullptr);
  testFanProfile();
  testRestartFan();
  testFanWithProtection();
  testTrayKeepsTheOvenNumbers();
  testWrongNumbersAndProtection();
  testTransitTime();
  if (failures) {
    std::printf("THERMAL CLOSED-LOOP: %d FAILED\n", failures);
    return 1;
  }
  std::printf("THERMAL CLOSED-LOOP: ALL PASSED\n");
  return 0;
}
