// Closed-loop tests: the REAL Thermal V3 controller (MAYAP_INDUSTRIAL_v1_0_0/thermal_v3.h) and the REAL E115 rule
// (MAYAP_INDUSTRIAL_v1_0_0/heater_response_watch.h) drive simulated ovens (tests/thermal-sim.h) through the disturbances of a real batch.
//
//  1. The oven of the 29/10 batch, fitted to the machine's own log: the 2.3.4 controller + E115 rule reproduce the failure of that day
//     (the oven stalls under SP and E115 cuts the heater, 1445 s after the door was opened on the machine); the 2.4.0 pair does not.
//  2. A family of ovens that differ in air mass, heater delay, wall loss, egg mass and coupling (12 hand-made + generated ones): the first
//     climb (also of a controller that has been idle since power-up, and of an empty oven), a tray of cold eggs of 0.5 / 1.0 times the eggs
//     inside, a door, the vent fan, a set-point step, a reboot in the middle of it all, a day of ordinary operation. Every scenario also runs
//     on the 2.3.4 pair (tests/fixtures) on the same ovens and prints its numbers under the new ones: the comparison in doc/THERMAL_V3_2_4_0.md
//     is this output.
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
      r.advance(3 * 3600.0);
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
    expectHealthy(st, "first climb, empty oven", {0.5, 0.30, 0.4, 0.0, false});
    CHECK(st.maxReachS <= old.maxReachS + 60.0, "empty oven: reached SP only after %.0f s (2.3.4: %.0f s)", st.maxReachS, old.maxReachS);
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
    expectHealthy(a, name, {0.5, 0.30, 0.4, 31.0, true});
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
    expectHealthy(st, name, {0.8, 1.2, 1.3, 35.5, false});
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

  // A heater that really is dead (SSR open, element gone) must still stop the batch, and not later than the 2.3.4 rule did.
  const Stats dead = now.deadHeater();
  const Stats deadOld = before.deadHeater();
  dead.print("settled, heater DEAD (must trip)");
  reference("settled, heater DEAD (must trip)", deadOld);
  CHECK(dead.trips == dead.n, "a dead heater must trip E115 on every oven (%d of %d)", dead.trips, dead.n);
  CHECK(dead.maxTripS <= 6000.0, "a dead heater must be stopped within 100 minutes (last one at %.0f s)", dead.maxTripS);
  CHECK(deadOld.trips == deadOld.n, "2.3.4 stops a dead heater too (%d of %d)", deadOld.trips, deadOld.n);
  CHECK(dead.maxTripS <= deadOld.maxTripS + 60.0, "the new rule must not find a dead heater later than the old one (%.0f vs %.0f s)", dead.maxTripS, deadOld.maxTripS);

  // The family must be able to reproduce the failure of 29/10 with the old pair, or it is not a test of the new one.
  CHECK(trayOld[1].trips * 2 >= trayOld[1].n, "the family must be able to reproduce the failure: 2.3.4 stopped %d of %d healthy ovens", trayOld[1].trips, trayOld[1].n);
}

int main() {
  std::printf("=== Thermal closed-loop tests ===\n");
  test29();
  testReboot29();
  testFamily(std::getenv("THERMAL_FULL_FAMILY") != nullptr);
  if (failures) {
    std::printf("THERMAL CLOSED-LOOP: %d FAILED\n", failures);
    return 1;
  }
  std::printf("THERMAL CLOSED-LOOP: ALL PASSED\n");
  return 0;
}
