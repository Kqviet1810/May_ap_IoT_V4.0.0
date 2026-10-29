#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_v3.h"
#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_v3_adapter.h"
using std::isfinite;
#include "../MAYAP_INDUSTRIAL_v1_0_0/sensor_format.h"

// Simple FOPDT Plant model for testing
struct TestPlant {
  double Pmax = 16000.0;
  double Cf = 400000.0;
  double UA = 200.0;
  double Tamb = 28.0;
  double td = 60.0; // 60s delay
  double tl = 30.0; // 30s lag
  double Ta = 30.0;
  double heater = 0.0;
  double sinkW = 0.0;   // a load the controller was never told about (cold eggs soaking up heat)
  std::vector<double> pipe;
  size_t pipeIdx = 0;

  TestPlant() {
    size_t steps = static_cast<size_t>(td / 0.1);
    pipe.assign(steps > 0 ? steps : 1, 0.0);
  }

  double step(double cmdPct, double dt = 0.1) {
    double pwr = (cmdPct / 100.0) * Pmax;
    double delayed = pipe[pipeIdx];
    pipe[pipeIdx] = pwr;
    pipeIdx = (pipeIdx + 1) % pipe.size();

    heater += (delayed - heater) * dt / tl;
    double loss = UA * (Ta - Tamb) + sinkW;
    double dTa = (heater - loss) / Cf;
    Ta += dTa * dt;
    return Ta;
  }
};

void testBasicCycle() {
  printf("[TEST] Basic Cycle & State Transitions...\n");
  thermalv3::Controller ctl;
  thermalv3::Input in;

  in.pv = 30.0f;
  in.sp = 37.5f;
  in.permit = true;
  in.capPct = 100.0f;
  in.dtS = 0.1f;

  // Step 1: When cold, should enter StClimb and command 100%
  thermalv3::Output out = ctl.step(in);
  assert(out.powerPct > 90.0f);
  assert(out.state == thermalv3::StClimb);

  // When permit is false, power must be 0 and state StFrozen
  in.permit = false;
  out = ctl.step(in);
  assert(out.powerPct == 0.0f);
  assert(out.state == thermalv3::StFrozen);
  printf("  -> Pass: Basic transitions OK\n");
}

void testDoorDetection() {
  printf("[TEST] Door Open Detection...\n");
  thermalv3::Controller ctl;
  thermalv3::Input in;
  in.sp = 37.5f;
  in.permit = true;
  in.capPct = 100.0f;
  in.dtS = 0.1f;

  // Simulate settled temperature at 37.5
  for (int i = 0; i < 300; ++i) {
    in.pv = 37.5f;
    ctl.step(in);
  }

  // Now simulate sudden door plunge: -0.1 C per second (rapid drop)
  float currPv = 37.5f;
  bool doorTriggered = false;
  for (int i = 0; i < 150; ++i) {
    currPv -= 0.015f; // -0.15 C/s
    in.pv = currPv;
    thermalv3::Output out = ctl.step(in);
    if (out.state == thermalv3::StDoor) {
      doorTriggered = true;
      // In door state, power must be clamped to mild hold
      assert(out.powerPct <= 30.0f);
    }
  }
  assert(doorTriggered);
  assert(ctl.doorDetected());
  printf("  -> Pass: Door open detected and power clamped OK\n");
}

void testEnergyInFlightLowerBound() {
  printf("[TEST] Energy-In-Flight Physical Lower Bound (No Panic Surge on Empty Pipe)...\n");
  thermalv3::Controller ctl;
  thermalv3::Input in;
  in.pv = 37.5f;
  in.sp = 37.5f;
  in.permit = true;
  in.capPct = 100.0f;
  in.dtS = 0.1f;

  // Let controller settle near setpoint
  for (int i = 0; i < 500; ++i) {
    ctl.step(in);
  }

  // Simulate a period where permit was false / heater off (pipe fills with zeroes)
  in.permit = false;
  for (int i = 0; i < 3000; ++i) { // 300 seconds off
    ctl.step(in);
  }

  // Now resume heating at 37.40 C (just 0.10 C below setpoint)
  in.permit = true;
  in.pv = 37.40f;
  thermalv3::Output out = ctl.step(in);

  // Without physical lower bound clamp, empty pipe would predict ~36.2C and surge to 80%+.
  // With physical lower bound (pv - 0.20C = 37.20C), error is at most 0.30C -> command must NOT surge above 40%!
  assert(ctl.predicted() >= in.pv - 0.201f);
  assert(out.powerPct < 40.0f);
  printf("  -> Pass: Predicted PV = %.2f C (bounded >= %.2f C), Command = %.1f%% (no panic surge)\n",
         ctl.predicted(), in.pv - 0.20f, out.powerPct);
}

void testPlantSimulation() {
  printf("[TEST] Full Plant Simulation from 30.0C to 37.5C (2 hours)...\n");
  TestPlant plant;
  thermalv3::Controller ctl;
  thermalv3::Input in;
  in.sp = 37.5f;
  in.permit = true;
  in.capPct = 100.0f;
  in.dtS = 0.1f;

  double maxTa = -100.0;
  double reachTime = -1.0;

  // Simulate 7200s (2 hours)
  for (int step = 0; step < 72000; ++step) {
    double t = step * 0.1;
    // Quantize sensor to 0.1 C
    float measPv = static_cast<float>(std::round(plant.Ta * 10.0) / 10.0);
    in.pv = measPv;

    thermalv3::Output out = ctl.step(in);
    plant.step(out.powerPct, 0.1);

    if (t > 300.0 && plant.Ta > maxTa) {
      maxTa = plant.Ta;
    }
    if (reachTime < 0.0 && plant.Ta >= 37.2) {
      reachTime = t;
    }
  }

  double overshoot = std::max(0.0, maxTa - 37.5);
  printf("  -> Results: ReachTime = %.1f min | Peak Ta = %.2f C | Overshoot = %.2f C | Final Ta = %.2f C\n",
         reachTime / 60.0, maxTa, overshoot, plant.Ta);

  assert(overshoot <= 0.40); // Strict incubation limit
  assert(std::fabs(plant.Ta - 37.5) <= 0.25); // Strict steady-state band
  printf("  -> Pass: Full plant simulation passed criteria!\n");
}

// Vent-line hold (shadow): nothing is learned while the heater is held at 0 %; the hold level fades only when PV stays at SP, never when PV falls.
void testShadowHold() {
  printf("[TEST] Vent-line hold: learning frozen, hold fades only while PV stays at SP...\n");
  auto run = [](double pvAfter, double minutes, float &holdOut, float &intOut) {
    thermalv3::Controller c;
    thermalv3::Input in;
    in.sp = 37.5f; in.pv = 37.4f; in.permit = true; in.capPct = 100.0f; in.dtS = 0.1f;
    for (int i = 0; i < 6000; ++i) c.step(in);                 // 10 min below SP: the controller learns a hold level
    const float holdBefore = c.pHold();
    in.permit = false; in.shadow = true; in.pv = static_cast<float>(pvAfter);
    for (int i = 0; i < static_cast<int>(minutes * 600.0); ++i) c.step(in);
    holdOut = c.pHold(); intOut = c.integral();
    return holdBefore;
  };
  float h = 0, itg = 0;
  float before = run(37.5, 1.0, h, itg);                         // 1 min: no checkpoint yet, nothing changes
  assert(std::fabs(h - before) < 0.01f);
  before = run(37.5, 12.0, h, itg);                              // PV held at SP for 12 min with the heater held off: hold fades
  assert(h < before * 0.2f);
  const float fadeFrom = before, fadeTo = h;
  before = run(37.0, 12.0, h, itg);                              // PV below SP - 0.05: hold must not fade
  assert(std::fabs(h - before) < 0.01f);
  printf("  -> Pass: hold %.1f%% -> %.1f%% when PV stays at SP, unchanged (%.1f%%) when it falls\n", fadeFrom, fadeTo, h);
}

// After the heater was held by a line above SP, the first command must come back near the hold level: the "pipe" was neutral, so no phantom deficit.
void testResumeAfterHold() {
  printf("[TEST] Resume after a held heater: command returns near the hold level...\n");
  for (float dQ : {0.1f, 0.01f}) {
    thermalv3::Controller c;
    thermalv3::Input in;
    in.sp = 37.5f; in.pv = 37.5f; in.dQ = dQ; in.permit = true; in.capPct = 100.0f; in.dtS = 0.1f;
    float u = 0.0f;
    for (int i = 0; i < 3000; ++i) u = c.step(in).powerPct;               // 5 min flat at SP
    const float before = u;
    in.permit = false; in.shadow = true;
    for (int i = 0; i < 1800; ++i) c.step(in);                            // 3 min held off, PV stays at SP
    in.permit = true; in.shadow = false;
    for (int i = 0; i < 10; ++i) u = c.step(in).powerPct;
    assert(u <= before + 3.0f);
    printf("  -> Pass (dQ %.2f): %.1f%% before the hold, %.1f%% right after\n", dQ, before, u);
  }
}

// 0.01 C preferred: a x10 module is read as x100 (3.75 C, outside the start envelope) until the decoder falls back and says so.
void testSensorFallback() {
  printf("[TEST] Sensor 0.01 preferred: fallback to 0.1 and the flag...\n");
  SensorFormatDecoder x100;                                               // a real x100 module
  x100.setRuntimeMode(2U);
  for (int i = 0; i < 6; ++i) x100.accept(3750U, 600U);
  assert(x100.locked() && x100.profile() == SensorProfile::X100RhX10 && !x100.fellBack() && std::fabs(x100.temperature() - 37.5f) < 1e-4f);
  SensorFormatDecoder x10;                                                // the module of this machine: 375 = 37.5 C
  x10.setRuntimeMode(2U);
  for (int i = 0; i < SensorFormatDecoder::FALLBACK_MISSES - 1; ++i) assert(!x10.accept(375U, 600U) && !x10.fellBack());
  bool ok = false;
  for (int i = 0; i < 20 && !ok; ++i) ok = x10.accept(375U, 600U);
  assert(ok && x10.fellBack() && x10.profile() == SensorProfile::X10RhX10 && std::fabs(x10.temperature() - 37.5f) < 1e-4f);
  SensorFormatDecoder strict;                                             // 0.1 chosen on purpose: never falls back
  strict.setRuntimeMode(1U);
  for (int i = 0; i < 30; ++i) strict.accept(3750U, 600U);
  assert(!strict.locked() && !strict.fellBack());
  x10.setRuntimeMode(2U); assert(x10.locked());                           // the same choice again changes nothing
  x10.setRuntimeMode(1U); assert(!x10.locked() && !x10.fellBack());      // a new choice starts over
  printf("  -> Pass\n");
}

// A load nobody announced. The oven holds 37.5 degC with ~12 % of the heater; a tray of cold eggs then soaks up 5 kW (31 % of the heater) for
// good. The 2.3.4 controller kept a hold level of 18 %, its energy-in-flight model promised heat that the eggs took, and it throttled the
// heater to 60 % with the oven 4 degC under SP (the batch of 29/10). The hold level must now follow the measured balance.
void testLearnsAnUnannouncedLoad() {
  printf("[TEST] Learns a load nobody announced (cold tray)...\n");
  TestPlant plant;
  plant.Ta = 28.0;
  thermalv3::Controller ctl;
  thermalv3::Input in;
  in.sp = 37.5f; in.permit = true; in.capPct = 100.0f; in.dtS = 0.1f;
  double holdBefore = 0.0, minTa = 100.0, minPowerNeedy = 100.0;
  bool needy = false;
  for (int step = 0; step < 3 * 3600 * 10; ++step) {
    const double t = step * 0.1;
    if (std::fabs(t - 7200.0) < 0.05) { holdBefore = ctl.pHold(); plant.sinkW = 5000.0; }
    in.pv = static_cast<float>(std::round(plant.Ta * 10.0) / 10.0);
    const thermalv3::Output out = ctl.step(in);
    plant.step(out.powerPct, 0.1);
    if (t > 7200.0) {
      minTa = std::min(minTa, plant.Ta);
      // once the controller has had time to read the balance (15 min), a heater working against an oven more than 2 degC under SP is at its ceiling
      if (t > 7200.0 + 900.0 && plant.Ta < 35.5) { needy = true; minPowerNeedy = std::min(minPowerNeedy, static_cast<double>(out.powerPct)); }
    }
  }
  printf("  -> hold %.1f %% before the load, %.1f %% after; lowest air %.2f; final air %.2f; lowest power while >2 degC under: %.0f %%\n",
         holdBefore, ctl.pHold(), minTa, plant.Ta, needy ? minPowerNeedy : -1.0);
  assert(ctl.pHold() > 35.0f && ctl.pHold() < 52.0f);                    // the true hold is 43 %
  assert(std::fabs(plant.Ta - 37.5) <= 0.25);                            // and the oven is back at SP
  assert(minTa > 33.0);                                                  // never worse than the load itself makes it
  assert(!needy || minPowerNeedy >= 90.0);
  printf("  -> Pass\n");
}

// A controller that restarts (update, power blip) next to SP has seen no start: it must not take the first 0.15 degC it sees for the
// transit time of the oven, and it must not move its gain prior.
void testRestartNextToSetpointKeepsPriors() {
  printf("[TEST] Restart next to SP: no false identification...\n");
  thermalv3::Controller ctl;
  thermalv3::Input in;
  in.sp = 37.5f; in.permit = true; in.capPct = 100.0f; in.dtS = 0.1f;
  const float kPrior = ctl.kAir(), tauPrior = ctl.tauTransit();
  uint32_t x = 12345U;
  for (int step = 0; step < 2 * 3600 * 10; ++step) {
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    const float wobble = (static_cast<int>(x % 7U) - 3) * 0.05f;           // +-0.15 degC of wander, 0.1 degC steps
    in.pv = std::round((37.3f + wobble + 0.2f * static_cast<float>(std::sin(step * 0.0005))) * 10.0f) / 10.0f;
    ctl.step(in);
  }
  assert(!ctl.identified());
  assert(ctl.kAir() == kPrior && ctl.tauTransit() == tauPrior);
  printf("  -> Pass: identified=%d kAir=%.5f tau=%.0f s\n", ctl.identified() ? 1 : 0, ctl.kAir(), ctl.tauTransit());
}

// On a real start the stopwatch and the gain capture do run, and their results are inside the physical limits.
void testStartIdentifiesTheOven() {
  printf("[TEST] A real start identifies delay and gain inside their limits...\n");
  TestPlant plant;
  thermalv3::Controller ctl;
  thermalv3::Input in;
  in.sp = 37.5f; in.permit = true; in.capPct = 100.0f; in.dtS = 0.1f;
  for (int step = 0; step < 20 * 60 * 10; ++step) {
    in.pv = static_cast<float>(std::round(plant.Ta * 10.0) / 10.0);
    plant.step(ctl.step(in).powerPct, 0.1);
  }
  const double trueK = plant.Pmax / 100.0 / plant.Cf;                    // degC/s per % of the heater
  printf("  -> identified=%d tau=%.0f s kAir=%.5f (true %.5f)\n", ctl.identified() ? 1 : 0, ctl.tauTransit(), ctl.kAir(), trueK);
  assert(ctl.identified());
  assert(ctl.tauTransit() >= 90.0f && ctl.tauTransit() <= 220.0f);
  assert(ctl.kAir() > 0.5 * trueK && ctl.kAir() < 1.3 * trueK);
}

// Whatever the temperature does, the learned hold level, the integral and the command stay inside their limits and finite.
void testBoundsUnderHostileInput() {
  printf("[TEST] Hold level, integral and command stay bounded under hostile input...\n");
  thermalv3::Controller ctl;
  thermalv3::Input in;
  in.sp = 37.5f; in.permit = true; in.capPct = 80.0f; in.dtS = 0.1f;
  uint32_t x = 987654321U;
  float pv = 37.0f;
  for (int step = 0; step < 4 * 3600 * 10; ++step) {
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    if (step % 600 == 0) pv = 30.0f + static_cast<float>(x % 1000U) * 0.01f;      // jumps anywhere in 30 .. 40 degC
    else pv += (static_cast<int>(x % 5U) - 2) * 0.01f;
    in.pv = pv;
    in.ventFan = (step / 3000) % 3 == 0;
    in.shadow = (step / 5000) % 7 == 3;
    in.permit = !in.shadow && (step / 7000) % 11 != 5;
    if (step % 9001 == 0) in.pv = NAN;                                                // a lost sample now and then
    const thermalv3::Output out = ctl.step(in);
    assert(std::isfinite(out.powerPct) && out.powerPct >= 0.0f && out.powerPct <= 80.0f);
    assert(ctl.pHold() >= 0.0f && ctl.pHold() <= thermalv3::HOLD_MAX_PCT);
    assert(std::fabs(ctl.integral()) <= thermalv3::I_CLAMP + 0.001f);
    assert(std::isfinite(ctl.kAir()) && ctl.kAir() >= thermalv3::KAIR_MIN && ctl.kAir() <= thermalv3::KAIR_MAX);
    assert(ctl.tauTransit() >= 45.0f && ctl.tauTransit() <= thermalv3::TAU_MAX_S);
  }
  printf("  -> Pass\n");
}

int main() {
  printf("=========================================\n");
  printf("RUNNING THERMAL V3 CONTROLLER UNIT TESTS\n");
  printf("=========================================\n");
  testBasicCycle();
  testDoorDetection();
  testEnergyInFlightLowerBound();
  testPlantSimulation();
  testLearnsAnUnannouncedLoad();
  testRestartNextToSetpointKeepsPriors();
  testStartIdentifiesTheOven();
  testBoundsUnderHostileInput();
  testShadowHold();
  testResumeAfterHold();
  testSensorFallback();
  printf("=========================================\n");
  printf("ALL THERMAL V3 TESTS PASSED SUCCESSFULLY!\n");
  printf("=========================================\n");
  return 0;
}
