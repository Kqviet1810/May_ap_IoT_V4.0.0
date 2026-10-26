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

void testVentFeedforward() {
  printf("[TEST] Vent Fan Feedforward...\n");
  thermalv3::Controller ctl;
  thermalv3::Input in;
  in.pv = 37.5f;
  in.sp = 37.5f;
  in.permit = true;
  in.capPct = 100.0f;
  in.dtS = 0.1f;

  // Baseline settled command with vent OFF
  float baseCmd = 0.0f;
  for (int i = 0; i < 300; ++i) {
    baseCmd = ctl.step(in).powerPct;
  }

  // Now turn vent fan ON
  in.ventFan = true;
  float ventCmd = 0.0f;
  for (int i = 0; i < 60; ++i) {
    ventCmd = ctl.step(in).powerPct;
  }

  // Vent power should increase by approx +10% to +12%
  assert(ventCmd > baseCmd + 8.0f);
  printf("  -> Pass: Vent feedforward offset verified (+%.1f%%)\n", ventCmd - baseCmd);
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

  assert(overshoot <= 0.35); // Strict incubation limit
  assert(std::fabs(plant.Ta - 37.5) <= 0.15); // Strict steady-state band
  printf("  -> Pass: Full plant simulation passed criteria!\n");
}

int main() {
  printf("=========================================\n");
  printf("RUNNING THERMAL V3 CONTROLLER UNIT TESTS\n");
  printf("=========================================\n");
  testBasicCycle();
  testDoorDetection();
  testVentFeedforward();
  testPlantSimulation();
  printf("=========================================\n");
  printf("ALL THERMAL V3 TESTS PASSED SUCCESSFULLY!\n");
  printf("=========================================\n");
  return 0;
}
