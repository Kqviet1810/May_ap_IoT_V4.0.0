// Launch (accelerate-then-brake) acceptance on a reduced realistic grid (SOFTWARE / SIMULATION ONLY, not a physical qualification).
// The production heating route with the real startup controller, a 0.01 / 0.1 C probe, ovens of G 0.008-0.025 C/s, dead time 100-180 s.
//   batch 1: nothing known (the controller measures gain, delay, drift, hold) -> must reach SP - 0.3 C without ever crossing High.
//   batch 2: the plan of batch 1 is loaded -> must launch at once: SP - 1 C well inside 20 minutes, no overshoot beyond 0.5 C.
//   load change: the same plan on a oven whose thermal mass is half / double (a different number of egg trolleys) -> never High,
//   overshoot bounded (the gain is measured again on every launch, the plan is only a prior).
#include "thermal-adaptive-sim.h"
#include <cstdio>
#include <sstream>
using namespace sim;

static unsigned checks = 0;
#define CHECK(cond) do { ++checks; if (!(cond)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); std::abort(); } } while (0)

struct Case { double G, dead, lag, hold, res, start; };

static Plant makePlant(const Case &c, double capScale) {
  Plant p; p.eff = 1.0; p.capacity = 16000.0 / c.G * capScale; p.dead = c.dead; p.lag = c.lag; p.ambient = 25.0; p.resolution = c.res;
  p.loss = c.hold * 16000.0 / (37.5 - 25.0);    // the loss at SP is the hold: independent of the load
  return p;
}
static Scenario makeScenario(const Case &c) {
  Scenario sc; sc.sp = 37.5f; sc.durationS = 3600; sc.mode = Mode::Adaptive; sc.metricsFromS = 3000; sc.startTemp = c.start;
  sc.gainsSet = true; sc.kp = 60.0 / (c.G * (c.dead + c.lag)); sc.ki = sc.kp / 250.0f; sc.kd = 0.0f;   // gains of an AutoTune of this plant
  return sc;
}


// A door opened in the hold drops PV by 1-5 C at once. The controller must push at once (the old symptom: the heater trickled while PV was
// far under SP) and brake in time (no overshoot). Seconds from the door until PV is back within 0.3 C of SP; `warm` = the controller restarted
// in the hold with the stored plan (the plan must survive the first door: it used to be thrown away as "the oven did not answer").
static double doorRecoveryS(const Case &c, double drop, bool warm, double &over, double &firstMinuteDuty) {
  const Plant p = makePlant(c, 1.0);
  const Scenario s1 = makeScenario(c);
  const Result r1 = run(p, s1);
  Scenario s2 = s1; s2.planSet = true; s2.plan = r1.planOut; s2.startTemp = warm ? 37.4f : 32.0f;
  s2.event = "door"; s2.eventAtS = 3000; s2.eventValue = drop; s2.durationS = 3000 + 3000; s2.metricsFromS = 3000;
  std::stringstream trace; s2.trace = true; s2.traceOut = &trace; s2.label = "T";
  const Result r2 = run(p, s2);
  CHECK(r2.high == 0 && r2.emergency == 0);
  double back = -1, peak = -99, duty = 0; unsigned nDuty = 0; bool low = false; std::string line;
  while (std::getline(trace, line)) {
    double t = 0, temp = 0, fed = 0, sp = 0, d60 = 0; char mode[40];
    if (std::sscanf(line.c_str(), "T,%39[^,],%lf,%lf,%lf,%lf,%lf", mode, &t, &temp, &fed, &sp, &d60) != 6 || t < 3000) continue;
    if (temp < 37.2) low = true;
    if (low && back < 0 && temp >= 37.2) back = t - 3000;
    if (t > 3060) peak = std::max(peak, temp);
    if (t >= 3030 && t <= 3150) { duty += d60; ++nDuty; }
  }
  over = peak - 37.5; firstMinuteDuty = nDuty ? duty / nDuty : 0;
  return back;
}

int main() {
  const Case cases[] = {
    {0.012, 160, 40, 0.20, 0.10, 32},   // the oven of the field report
    {0.015, 130, 50, 0.10, 0.10, 32},
    {0.008, 100, 40, 0.12, 0.01, 29},   // weak heater
    {0.016, 140, 40, 0.28, 0.10, 32},   // high hold
    {0.025, 100, 40, 0.12, 0.01, 32},   // strong heater
    {0.025, 180, 40, 0.20, 0.10, 29},   // strong heater, long delay
    {0.008, 180, 40, 0.20, 0.01, 32},   // weak heater, long delay
    {0.012, 120, 40, 0.28, 0.01, 29},
  };
  unsigned n = 0;
  for (const Case &c : cases) {
    const Plant p = makePlant(c, 1.0);
    const Scenario s1 = makeScenario(c);
    const Result r1 = run(p, s1);
    CHECK(r1.high == 0 && r1.emergency == 0);
    CHECK(r1.riseS03 >= 0 && r1.riseS03 <= 3500);          // batch 1 (probing) gets there inside the hour
    CHECK(r1.peakAfterRise - 37.5 <= 0.6);
    CHECK(r1.planOffered);                                   // and leaves a plan
    Scenario s2 = s1; s2.planSet = true; s2.plan = r1.planOut;
    const Result r2 = run(p, s2);
    CHECK(r2.high == 0 && r2.emergency == 0);
    CHECK(r2.riseS10 >= 0 && r2.riseS10 <= 1200);          // batch 2: SP - 1 C within 20 min
    CHECK(r2.riseS03 >= 0 && r2.riseS03 <= 2400);
    CHECK(r2.peakAfterRise - 37.5 <= 0.5);
    for (double scale : {0.5, 2.0}) {                         // a different load with the same plan
      const Result r3 = run(makePlant(c, scale), s2);
      CHECK(r3.high == 0 && r3.emergency == 0);
      CHECK(r3.peakAfterRise - 37.5 <= 0.6);
    }
    ++n;
  }
  // The profile ventilation (18 % of a 40 minute cycle, 60 W/C when open) is held back during the warm-up (WARMUP_VENT_*): the climb to SP - 1 C
  // is not slowed by it, with or without a plan. (Without the hold the open fan cost minutes on every cycle.)
  for (const Case &c : {cases[0], cases[3], cases[5]}) {
    Plant pv = makePlant(c, 1.0); pv.ventG = 60;
    Scenario sv = makeScenario(c); sv.ventProfile = true; sv.ventCycleMin = 40; sv.ventDutyPct = 18;
    const Result closed = run(makePlant(c, 1.0), makeScenario(c));
    const Result vented = run(pv, sv);
    CHECK(vented.high == 0 && vented.emergency == 0);
    CHECK(vented.riseS10 >= 0 && vented.riseS10 <= closed.riseS10 + 60);
    Scenario sv2 = sv; sv2.planSet = true; sv2.plan = closed.planOut;
    if (closed.planOffered) {
      Scenario sc2 = makeScenario(c); sc2.planSet = true; sc2.plan = closed.planOut;
      const Result closed2 = run(makePlant(c, 1.0), sc2), vented2 = run(pv, sv2);
      CHECK(vented2.high == 0 && vented2.emergency == 0);
      CHECK(vented2.riseS10 >= 0 && vented2.riseS10 <= closed2.riseS10 + 60);
    }
  }
  for (const Case &c : {cases[0], cases[1], cases[3], cases[4]}) for (double drop : {1.0, 3.0}) {
    double over = 0, duty = 0;
    const double net = c.G * (1.0 - c.hold * 0.7), ideal = c.dead + 20 + (drop - 0.3) / net;
    const double back = doorRecoveryS(c, drop, false, over, duty);
    CHECK(duty >= (drop >= 3.0 ? 0.6 : 0.35));             // pushes within the first two minutes (well above the hold duty of 0.1-0.28; it trickled before)
    CHECK(back >= 0 && back <= 2.4 * ideal);                // back within 0.3 C of SP, not much slower than physics allows
    CHECK(over <= 0.35);                                    // and braked in time
    if (drop == 3.0) {                                      // restart in the hold with the plan, then a door
      const double backWarm = doorRecoveryS(c, drop, true, over, duty);
      CHECK(duty >= 0.6);
      CHECK(backWarm >= 0 && backWarm <= 4.0 * ideal);
    }
  }
  // An oven in a room warmer than its set point (here 37.9 C against SP 37.5 C: PV creeps +0.1 C in two hours at 0 percent): with the heater idle PV still climbs above SP.
  // The controller must not heat it (no overshoot of its own making), must remember that this oven cannot cool by itself (the plan carries it),
  // and the next batch must stay clear of High.
  {
    Plant p; p.eff = 1.0; p.capacity = 16000.0 / 0.012; p.dead = 140; p.lag = 40; p.ambient = 37.9; p.resolution = 0.1;
    p.loss = 0.5 * 16000.0 / 100.0;                       // 80 W/C: equilibrium without heating 37.9 C
    Case c{0.012, 140, 40, 0.0, 0.10, 32};
    const Scenario s1 = makeScenario(c);
    Scenario s1l = s1; s1l.durationS = 9000; s1l.metricsFromS = 8000;
    const Result r1 = run(p, s1l);
    CHECK(r1.high == 0 && r1.emergency == 0);
    CHECK(r1.planOffered);
    CHECK(r1.planOut.idleOverC >= 0.08f && r1.planOut.idleOverC <= 0.5f);   // remembered: PV crept above SP with the heater idle and nothing on its way
    Scenario s2 = s1l; s2.planSet = true; s2.plan = r1.planOut;
    const Result r2 = run(p, s2);
    CHECK(r2.high == 0 && r2.emergency == 0);
  }
  std::printf("Launch: %u realistic ovens, batch 1 without a plan, batch 2 with the stored plan, half / double thermal mass: High=0, overshoot bounded (%u checks) PASS\n", n, checks);
  return 0;
}
