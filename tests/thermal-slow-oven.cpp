// Slow, well insulated oven: weak heater, long delay, very small hold duty, 0.1 C probe (SOFTWARE / SIMULATION ONLY).
//
// The plant is the oven of the field report (Kh ~0.014 C/s at 100 %, apparent delay ~200 s = dead 150 + lag 50, hold ~1.9 % of the bank at
// 37.5 C, room 28 C). The controller is the production heating route (see thermal-adaptive-sim.h); only the oven is a model.
//
// What it pins: the startup energy brake is sized for a light 16 kW chamber. On this oven it used to keep a permanent offset under the
// setpoint (PV sat at SP - 0.3 C for hours with ~2 % duty and a hold that was never learned, in every mode). A running oven (warm start)
// with a stale, unqualified profile must now reach the setpoint and hold it, with and without Adaptive / Smart Thermal.
#include "thermal-adaptive-sim.h"

using namespace sim;

static unsigned checks = 0;
#define CHECK(cond) do { ++checks; if (!(cond)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); std::abort(); } } while (0)

static Plant userOven(double hold) {
  Plant p;
  p.eff = 0.525; p.capacity = 600000; p.dead = 150; p.lag = 50; p.ambient = 28; p.resolution = 0.1;
  p.loss = hold * 16000.0 * p.eff / (37.5 - p.ambient);       // hold = loss * (SP - ambient) / (16000 * eff)
  return p;
}

static Scenario scenario(Mode mode, double startTemp, bool userGains) {
  Scenario sc;
  sc.sp = 37.5f; sc.durationS = 4 * 3600.0; sc.mode = mode; sc.startTemp = startTemp;
  if (userGains) { sc.gainsSet = true; sc.kp = 35.0f; sc.ki = 0.1f; sc.kd = 0.0f; }
  // the profile the field machine showed: seed gain/delay, partial confidence, nothing measured
  sc.seedSet = true; sc.seed = MayapThermal::defaultProfile();
  sc.seed.heaterGain = 0.03f; sc.seed.heaterDelaySec = 45.0f; sc.seed.holdPowerPct = 0.0f; sc.seed.coastRiseC = 0.5f; sc.seed.confidence = 33;
  MayapThermal::sanitizeProfile(sc.seed);
  return sc;
}

int main() {
  const Mode modes[] = {Mode::Baseline, Mode::Adaptive, Mode::Smart};
  unsigned runs = 0;
  for (double hold : {0.015, 0.019, 0.03}) {
    const Plant p = userOven(hold);
    for (Mode mode : modes) for (double start : {37.0, 36.5}) for (bool gains : {false, true}) {
      Scenario sc = scenario(mode, start, gains);
      const Result r = run(p, sc);
      ++runs;
      // never a High / Emergency, never a real overshoot
      CHECK(r.high == 0 && r.emergency == 0 && r.overshoot <= 0.30);
      // the oven reaches the setpoint (it used to sit 0.3 C under it for the whole run) and holds it in the last 30 minutes
      CHECK(r.riseS >= 0 && r.riseS <= 2.0 * 3600.0);
      CHECK(r.mae <= 0.12 && r.p95 <= 0.20);
    }
    // cold start: the first heat-up of an unlearned oven, every mode
    for (Mode mode : modes) {
      Scenario sc = scenario(mode, NAN, false);
      sc.durationS = 6 * 3600.0;
      const Result r = run(p, sc);
      ++runs;
      CHECK(r.high == 0 && r.emergency == 0);
      // the unlearned legacy brake has always overshot this first heat-up by 0.3-0.4 C (not changed here); Adaptive / Smart stay under 0.30
      CHECK(r.overshoot <= (mode == Mode::Baseline ? 0.45 : 0.30));
      CHECK(r.mae <= 0.15);
    }
  }
  // After a Smart AutoTune the profile carries a MEASURED hold (1.9 %) and the gains are the tuned ones. The hold is then fed forward from 1 %,
  // so the integral no longer has to carry it. (The startup brake closes for a minute after every upward 0.1 C probe step and the PID's integral
  // limit follows that transient ceiling, so the integral is wiped each time; without the feed-forward the loop ran proportional-only and parked
  // about hold / Kp = 0.15 C under the set point.) 12 h with a door opening at hour 5.
  for (double dead : {150.0, 210.0}) {
    Plant p = userOven(0.019); p.dead = dead;
    Scenario sc = scenario(Mode::Adaptive, NAN, false);
    sc.durationS = 12 * 3600.0; sc.gainsSet = true; sc.kd = 0.0f;
    sc.kp = dead > 180 ? 12.0f : 15.2f; sc.ki = dead > 180 ? 0.0046f : 0.0074f;
    sc.seed = MayapThermal::defaultProfile();
    sc.seed.heaterGain = 0.0135f; sc.seed.heaterDelaySec = static_cast<float>(dead + 50.0); sc.seed.holdPowerPct = 1.9f;
    sc.seed.coastRiseC = dead > 180 ? 3.5f : 2.5f; sc.seed.confidence = 67; MayapThermal::sanitizeProfile(sc.seed);
    sc.event = "door"; sc.eventAtS = 5 * 3600.0; sc.eventValue = 1.5;
    const Result r = run(p, sc);
    ++runs;
    CHECK(r.high == 0 && r.emergency == 0 && r.overshoot <= 0.15);
    CHECK(r.mae <= 0.08 && r.p95 <= 0.12);
  }
  // Medium-delay oven with a few percent of hold and a strong heater (field log of 2026-10-20: PV sat 0.5 C under SP for ~1000 s, hold
  // learned 0.3 percent instead of ~4 percent, then ripple around SP - 0.06 C). The hold used to be measured only after six minutes in a
  // row of flat samples, which a 0.1 C probe never allows; the window drift test measures it. The run must reach SP and hold it.
  for (double hold : {0.04, 0.05}) for (Mode mode : modes) {
    Plant p; p.eff = 1.0; p.capacity = 600000; p.dead = 60; p.lag = 30; p.ambient = 28; p.resolution = 0.1; p.ventG = 40;
    p.loss = hold * 16000.0 * p.eff / (37.5 - p.ambient);
    Scenario sc = scenario(mode, 37.0, true);
    sc.kp = 30.0f; sc.ventProfile = true; sc.metricsFromS = 5400;
    const Result r = run(p, sc);
    ++runs;
    CHECK(r.high == 0 && r.emergency == 0 && r.overshoot <= 0.15);
    CHECK(r.riseS >= 0 && r.riseS <= 2.0 * 3600.0);
    CHECK(r.mae <= 0.12 && r.p95 <= 0.20);
  }
  // Weak heater on a light, fine-probe chamber started just under the set point (grid test of 2026-10-20): the heat surfaces as a climb of
  // 0.0005 C/s, never the 0.003 C/s "first rise" slope, so the hold learner, the delay estimate and the measured heater gain stayed switched
  // off and PV sat 0.5 C under SP for the whole run. A rise of 0.25 C since the first heat counts as the first rise as well.
  for (double start : {36.0, 32.0}) for (Mode mode : {Mode::Baseline, Mode::Adaptive}) {
    Plant p; p.eff = 0.25; p.capacity = 300000; p.dead = 10; p.lag = 10; p.ambient = 25; p.resolution = 0.01; p.ventG = 0;
    p.loss = 0.03 * 16000.0 * p.eff / (37.5 - 25.0);
    Scenario sc; sc.sp = 37.5f; sc.durationS = 3 * 3600.0; sc.mode = mode; sc.startTemp = start; sc.metricsFromS = 5400;
    const Result r = run(p, sc);
    ++runs;
    CHECK(r.high == 0 && r.emergency == 0 && r.overshoot <= 0.30);
    CHECK(r.riseS >= 0 && r.riseS <= 1.5 * 3600.0);
    CHECK(r.mae <= (mode == Mode::Baseline ? 0.20 : 0.10));
  }
  // 90 s dead time, hot room, 0.1 C probe (holdout rows h107 / h119 / h215): a wrong learned hold feed-forward during the climb removed the
  // braking when the measured-gain relief used a 2x reserve; the 4x reserve keeps the overshoot below the High threshold.
  for (double loss : {150.0, 240.0}) {
    Plant p; p.eff = 0.85; p.capacity = 900000; p.loss = loss; p.dead = 90; p.lag = 20; p.ambient = 31; p.resolution = 0.1;
    Scenario sc; sc.sp = 37.5f; sc.durationS = 4 * 3600.0; sc.mode = Mode::Adaptive;
    const Result r = run(p, sc);
    ++runs;
    CHECK(r.high == 0 && r.emergency == 0 && r.overshoot <= 0.60);
  }
  std::printf("Slow oven (Kh 0.014, delay 200 s, hold 1.5-3 %%, 0.1 C probe): %u closed-loop runs, warm and cold start, legacy/Adaptive/Smart: reach SP, hold it, High=0 Emergency=0 (%u checks) PASS\n", runs, checks);
  return 0;
}
