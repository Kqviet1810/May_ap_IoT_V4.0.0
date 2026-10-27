#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_v3.h"
#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_v3_adapter.h"

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
    double loss = UA * (Ta - Tamb);
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

int main() {
  printf("=========================================\n");
  printf("RUNNING THERMAL V3 CONTROLLER UNIT TESTS\n");
  printf("=========================================\n");
  testBasicCycle();
  testDoorDetection();
  testEnergyInFlightLowerBound();
  testPlantSimulation();
  testShadowHold();
  printf("=========================================\n");
  printf("ALL THERMAL V3 TESTS PASSED SUCCESSFULLY!\n");
  printf("=========================================\n");
  return 0;
}
