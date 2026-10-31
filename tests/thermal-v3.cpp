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
  printf("[TEST] Vent-line hold (heater held at 0 %% by the safety margin): nothing is learned while it lasts...\n");
  auto run = [](double pvAfter, double minutes, float &holdOut, float &intOut) {
    thermalv3::Controller c;
    thermalv3::Input in;
    in.sp = 37.5f; in.pv = 37.4f; in.permit = true; in.capPct = 100.0f; in.dtS = 0.1f;
    for (int i = 0; i < 6000; ++i) c.step(in);                 // 10 min below SP: the controller has a hold level
    const float holdBefore = c.pHold();
    in.permit = false; in.shadow = true; in.pv = static_cast<float>(pvAfter);
    for (int i = 0; i < static_cast<int>(minutes * 600.0); ++i) {
      const thermalv3::Output o = c.step(in);
      assert(o.powerPct == 0.0f && o.state == thermalv3::StFrozen);
    }
    holdOut = c.pHold(); intOut = c.integral();
    return holdBefore;
  };
  float h = 0, itg = 0;
  for (double pv : {37.5, 37.0, 37.9}) {
    for (double minutes : {1.0, 12.0}) {
      const float before = run(pv, minutes, h, itg);
      assert(std::fabs(h - before) < 0.01f);                     // the hold level is exactly what it was: the held heater says nothing about the oven
    }
  }
  printf("  -> Pass: the hold level is unchanged after 1 and 12 minutes of hold, at, under and over SP\n");
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

// On the machine the controller runs from power-up with the permit off (it pushes zeros into its ring and its history), and the batch starts
// some time later. Two things must not depend on how long that pause was:
//  - the energy the model counts as on its way is the sum of the last pipeLen_ commands, also after the identification of the oven at that
//    start has changed the length of the ring (the write position used to be left beyond the shorter ring, and the running sum then drifted
//    for good: the energy in flight was off by up to 100 percent-seconds in about one start in three);
//  - the hold level learned from the energy balance: the power steps from nothing to full at the start, and a window of power that contains
//    that step says nothing about the hold level (it used to fall to about nothing during the climb, and the first approach to SP was slower).
void testStartAfterAPause() {
  printf("[TEST] Start after a pause: energy in flight = the commands sent, hold level not thrown off by the step...\n");
  int runs = 0, identified = 0;
  double worst = 0.0, lowestHold = 1e9;
  for (int pause = 0; pause < 420; pause += 31) {
    TestPlant plant;
    thermalv3::Controller ctl;
    thermalv3::Input in;
    in.sp = 37.5f; in.permit = false; in.capPct = 100.0f; in.dtS = 1.0f;   // one control step per second: one command per ring entry
    std::vector<int> sent;
    int identifiedAt = -1;
    for (int s = 0; s < pause + 2400; ++s) {
      in.permit = s >= pause;                                              // the batch starts after `pause` seconds
      in.pv = static_cast<float>(std::round(plant.Ta * 10.0) / 10.0);
      const float holdBefore = ctl.pHold();                                // the hold level the prediction of this step is computed with
      const thermalv3::Output out = ctl.step(in);
      for (int k = 0; k < 10; ++k) plant.step(out.powerPct, 0.1);
      sent.push_back(static_cast<int>(std::round(out.powerPct)));
      if (s > pause) lowestHold = std::min(lowestHold, static_cast<double>(ctl.pHold()));
      if (identifiedAt < 0 && ctl.identified()) identifiedAt = s;
      if (identifiedAt < 0) continue;
      const int len = static_cast<int>(ctl.tauTransit() * 1.35f);          // the ring length the identification set
      if (s < identifiedAt + len + 2) continue;                            // until the ring holds only commands that were really sent
      int sum = 0;
      for (int k = s - len; k < s; ++k) sum += sent[static_cast<size_t>(k)];
      const float expected = std::max(in.pv - 0.20f, in.pv + ctl.kAir() * (static_cast<float>(sum) - static_cast<float>(len) * holdBefore));
      worst = std::max(worst, static_cast<double>(std::fabs(ctl.predicted() - expected)));
    }
    ++runs;
    if (identifiedAt >= 0) ++identified;
  }
  printf("  -> %d of %d starts identified the oven; largest gap between the prediction and the commands sent: %.5f degC; lowest hold level %.1f %%\n",
         identified, runs, worst, lowestHold);
  assert(identified == runs);
  assert(worst < 5.0e-4);
  assert(lowestHold > 8.0);                                                // the true hold of this oven is 12 %, the prior 18 %
  printf("  -> Pass\n");
}

// The controller runs for weeks without a restart (the machine is on from power-up to the next power cut), and every slope it uses, the door
// detection among them, is a difference of two readings of its own clock. A float clock that accumulates 0.1 s steps runs 25 % fast after about
// 6 days and stands still after about 20: the slope read 0 and a plunge of the door was never seen. The clock must keep counting (here the clock
// is aged in 1 s steps with the permit off, which is how a machine waits for its batch, and then the real 0.1 s steps follow).
void testDoorPlungeAfterWeeksOfUptime() {
  printf("[TEST] A door plunge is still measured after weeks of uptime...\n");
  const double days[3] = {3.0, 8.0, 30.0};
  for (int k = 0; k < 3; ++k) {
    thermalv3::Controller ctl;
    thermalv3::Input in;
    in.sp = 37.5f; in.pv = 37.5f; in.capPct = 100.0f; in.permit = false; in.dtS = 1.0f;
    for (long second = 0; second < static_cast<long>(days[k] * 86400.0); ++second) ctl.step(in);
    in.permit = true; in.dtS = 0.1f;
    for (int i = 0; i < 3000; ++i) ctl.step(in);                           // five minutes at SP, the real step
    float steepest = 0.0f, pv = 37.5f;
    bool door = false;
    for (int i = 0; i < 1000; ++i) {                                        // 100 s of -0.02 degC/s
      pv -= 0.002f;
      in.pv = pv;
      ctl.step(in);
      steepest = std::min(steepest, ctl.slopeMks());
      door = door || ctl.doorDetected();
    }
    printf("  -> after %2.0f days: steepest slope %.1f mK/s (true -20.0), door seen: %s\n", days[k], steepest, door ? "yes" : "NO");
    assert(door);
    assert(steepest < -19.0f && steepest > -21.0f);
  }
  printf("  -> Pass\n");
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
    in.shadow = (step / 5000) % 7 == 3;
    in.permit = !in.shadow && (step / 7000) % 11 != 5;
    if (step % 9001 == 0) in.pv = NAN;                                                // a lost sample now and then
    const thermalv3::Output out = ctl.step(in);
    assert(std::isfinite(out.powerPct) && out.powerPct >= 0.0f && out.powerPct <= 80.0f);
    assert(ctl.pHold() >= 0.0f && ctl.pHold() <= thermalv3::HOLD_MAX_PCT);
    assert(std::fabs(ctl.integral()) <= thermalv3::I_CLAMP + 0.001f);
    assert(std::isfinite(ctl.kAir()) && ctl.kAir() >= thermalv3::KAIR_MIN && ctl.kAir() <= thermalv3::KAIR_MAX);
    assert(ctl.tauTransit() >= thermalv3::TAU_MIN_S && ctl.tauTransit() <= thermalv3::TAU_MAX_S);
  }
  printf("  -> Pass\n");
}

// The heater is NOT cut by the exhaust fan (the two small fans of this machine take little heat away): when PV stands one sensor step over
// SP and does not fall, the controller trims its own hold level (after a judging window longer than the transit time) and keeps doing so until
// PV comes down; a PV that is coming down (fast enough) stops the trim. The harder cut above SP+0.3 is the machine's own safety margin.
void testOverheatTrimsTheHoldLevel() {
  printf("[TEST] Overheat that does not fall: the hold level is trimmed, never cut by the controller; a falling PV stops the trim...\n");
  for (float dQ : {0.1f, 0.01f}) {
    thermalv3::Controller c;
    thermalv3::Input in;
    in.sp = 37.5f; in.dQ = dQ; in.permit = true; in.capPct = 100.0f; in.dtS = 0.1f;
    in.pv = 37.1f;
    for (int i = 0; i < 6000; ++i) c.step(in);                             // builds a hold level below SP
    const float highHold = c.pHold();
    assert(highHold > 20.0f);
    const float overPv = (dQ >= 0.09f) ? 37.60f : 37.58f;                   // one sensor step over SP
    in.pv = overPv;
    for (int i = 0; i < 1100; ++i) c.step(in);                              // 110 s: inside the judging window, nothing is trimmed yet
    assert(std::fabs(c.pHold() - highHold) < 0.5f);
    float cmd = 0.0f;
    for (int i = 0; i < 6000; ++i) cmd = c.step(in).powerPct;               // ten more minutes stuck over SP
    assert(c.pHold() < highHold - 8.0f);                                    // 0.025 %/s over ~9 minutes
    assert(cmd <= c.pHold() + 0.5f);                                        // the command is below the hold level: P and I only subtract
    // PV now falls 0.1 degC in 3 minutes: faster than the "not falling" limit, the trim stops
    const float holdAtFall = c.pHold();
    float pv = overPv;
    for (int i = 0; i < 1800; ++i) { pv -= 0.1f / 1800.0f; in.pv = pv; c.step(in); }
    assert(c.pHold() > holdAtFall - 1.0f);
    printf("  -> Pass (dQ %.2f): hold %.1f%% -> %.1f%% in ten minutes over SP; %.1f%% after a 3-minute fall\n", dQ, highHold, holdAtFall, c.pHold());
  }
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
  testStartAfterAPause();
  testDoorPlungeAfterWeeksOfUptime();
  testBoundsUnderHostileInput();
  testShadowHold();
  testResumeAfterHold();
  testSensorFallback();
  testOverheatTrimsTheHoldLevel();
  printf("=========================================\n");
  printf("ALL THERMAL V3 TESTS PASSED SUCCESSFULLY!\n");
  printf("=========================================\n");
  return 0;
}
