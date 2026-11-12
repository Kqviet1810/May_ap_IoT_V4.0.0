#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>
#define private public   // tests only: the hold level is set directly (the controller has no way to be given one - nothing is kept across a restart)
#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_v3.h"
#undef private
#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_v3_adapter.h"
using std::isfinite;
static void setHold(thermalv3::Controller &c, float h) { c.hold_ = h; c.derive(); }
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

  // Ten minutes settled at SV: the controller has a history to measure the slope on.
  for (int i = 0; i < 6000; ++i) {
    in.pv = 37.5f;
    ctl.step(in);
  }

  // A door: the air falls 0.012 degC/s (0.7 degC/min) for 150 s. The new law answers with the heater, not with a clamp: full power until the
  // heat in flight is enough to land on SV (the open door is only a load the heater has to beat).
  float currPv = 37.5f;
  bool doorTriggered = false;
  float peak = 0.0f;
  for (int i = 0; i < 1500; ++i) {
    currPv -= 0.0012f;
    in.pv = currPv;
    const thermalv3::Output out = ctl.step(in);
    if (out.state == thermalv3::StDoor) {
      doorTriggered = true;
      assert(out.powerPct >= ctl.pHold() - 0.01f);
    }
    if (out.powerPct > peak) peak = out.powerPct;
  }
  assert(doorTriggered);
  assert(ctl.doorDetected());
  assert(peak > ctl.pHold() + 10.0f);      // it does work against the door
  // While the door counts as open the load estimate is held (the door's loss is not a hold level).
  const float holdAtDoor = ctl.pHold();
  for (int i = 0; i < 300; ++i) { in.pv = currPv; ctl.step(in); }
  assert(std::fabs(ctl.pHold() - holdAtDoor) < 1.0f);
  printf("  -> Pass: Door detected, peak command %.0f %%, hold held at %.1f %%\n", peak, holdAtDoor);
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
  // The physical bound is what the load can cool the air in one transit time with nothing in the pipe: kAir x hold x tau (about 0.3 degC for the priors).
  assert(ctl.predicted() >= in.pv - ctl.kAir() * ctl.pHold() * ctl.tauTransit() - 0.01f);
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

// Heater held off by a line above SP (shadow). The controller records the power really given (0) and keeps observing: if PV stays at SP with nothing
// given, the load is zero and the hold level fades; if PV falls with nothing given, there is a load and the hold level must not fade.
void testShadowHold() {
  printf("[TEST] Heater held off: hold fades only while PV stays at SP, never when PV falls...\n");
  auto run = [](double pvAfter, double fallFactor, double minutes, float &holdOut) {
    thermalv3::Controller c;
    thermalv3::Input in;
    in.sp = 37.5f; in.pv = 37.4f; in.permit = true; in.capPct = 100.0f; in.dtS = 0.1f;
    for (int i = 0; i < 6000; ++i) c.step(in);                 // 10 min below SP: the controller learns a hold level
    const float holdBefore = c.pHold();
    // the air falls as fast as the hold level alone would cool it (times fallFactor) - the load is there, the heater is off
    const double fallPerS = fallFactor * static_cast<double>(c.kAir()) * static_cast<double>(holdBefore);
    in.permit = false; in.shadow = true;
    for (int i = 0; i < static_cast<int>(minutes * 600.0); ++i) { in.pv = static_cast<float>(pvAfter - fallPerS * (i / 10.0)); c.step(in); }
    holdOut = c.pHold();
    return holdBefore;
  };
  float h = 0;
  float before = run(37.5, 0.0, 12.0, h);                             // PV held at SP for 12 min with the heater off: no load, hold fades
  assert(h < before * 0.2f);
  const float fadeFrom = before, fadeTo = h;
  before = run(37.5, 1.0, 6.0, h);                               // PV falls at the rate the hold level cools the air with the heater off: a load, hold must not fade
  assert(h >= before - 0.5f);
  printf("  -> Pass: hold %.1f%% -> %.1f%% when PV stays at SP, %.1f%% -> %.1f%% when it falls\n", fadeFrom, fadeTo, before, h);
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
    // The pipe really was empty for 3 min (the heater was off and the history records what was given): the controller answers with the small deficit that
    // empty pipe means, at most kn x kAir x hold x tau (about 4 %) - not with a surge. (The 2.5.x law declared the pipe neutral instead: +3.)
    assert(u <= before + 6.0f);
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
  // A start next to SV has no climb to measure kAir on: it begins at the middle of the fleet (KAIR_WARM) and must never move off that by "identification".
  assert(ctl.kAir() == kPrior || ctl.kAir() == thermalv3::KAIR_WARM);
  assert(ctl.tauTransit() == tauPrior);
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
    std::vector<float> sent;
    int identifiedAt = -1;
    for (int s = 0; s < pause + 2400; ++s) {
      in.permit = s >= pause;                                              // the batch starts after `pause` seconds
      in.pv = static_cast<float>(std::round(plant.Ta * 10.0) / 10.0);
      const float holdBefore = ctl.pHold();                                // the hold level the prediction of this step is computed with
      const float kAirBefore = ctl.kAir(), tauBefore = ctl.tauTransit();   // ... and the gain and transit time (identification moves them after the step)
      const thermalv3::Output out = ctl.step(in);
      for (int k = 0; k < 10; ++k) plant.step(out.powerPct, 0.1);
      sent.push_back(out.powerPct);
      if (s > pause) lowestHold = std::min(lowestHold, static_cast<double>(ctl.pHold()));
      if (identifiedAt < 0 && ctl.identified()) identifiedAt = s;
      if (identifiedAt < 0) continue;
      const int len = static_cast<int>(thermalv3::PIPE_K * tauBefore);   // the window of power counted as still in flight
      if (s < identifiedAt + len + 2) continue;                            // until the ring holds only commands that were really sent
      float sum = 0.0f;
      for (int k = s - len; k < s; ++k) sum += sent[static_cast<size_t>(k)];
      const float raw = kAirBefore * thermalv3::LEAD_GAIN * (sum - static_cast<float>(len) * holdBefore);
      const float lo = -kAirBefore * holdBefore * tauBefore;                // nothing in the pipe: the load cools the air for one transit time
      const float inflightMax = kAirBefore * (100.0f - holdBefore) * tauBefore;
      if (in.sp - in.pv > 0.75f * inflightMax) continue;                           // far under SV the measured slope sets a floor under the prediction (checked by the rigs)
      const float expected = in.pv + std::min(1.2f * inflightMax, std::max(lo, raw));
      worst = std::max(worst, static_cast<double>(std::fabs(ctl.predicted() - expected)));
    }
    ++runs;
    if (identifiedAt >= 0) ++identified;
  }
  printf("  -> %d of %d starts identified the oven; largest gap between the prediction and the commands sent: %.5f degC; lowest hold level %.1f %%\n",
         identified, runs, worst, lowestHold);
  assert(identified == runs);
  assert(worst < 5.0e-4);
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
    assert(steepest < -17.5f && steepest > -21.0f);   // a least-squares slope over the window reads ~8 % under the true -20 mK/s; a stalled clock reads 0, a fast one -25 or -15
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
    in.ventFan = (step / 3000) % 3 == 0;
    in.shadow = (step / 5000) % 7 == 3;
    in.permit = !in.shadow && (step / 7000) % 11 != 5;
    if (step % 9001 == 0) in.pv = NAN;                                                // a lost sample now and then
    const thermalv3::Output out = ctl.step(in);
    assert(std::isfinite(out.powerPct) && out.powerPct >= 0.0f && out.powerPct <= 80.0f);
    assert(ctl.pHold() >= 0.0f && ctl.pHold() <= 100.0f);
    assert(std::isfinite(ctl.kAir()) && ctl.kAir() >= thermalv3::KAIR_PRIOR * thermalv3::KAIR_LO && ctl.kAir() <= thermalv3::KAIR_PRIOR * thermalv3::KAIR_HI);
    assert(ctl.tauTransit() >= thermalv3::TAU_PRIOR * thermalv3::TAU_LO && ctl.tauTransit() <= thermalv3::TAU_PRIOR * thermalv3::TAU_HI);
  }
  printf("  -> Pass\n");
}

// 2.9.9: the zero zone and the dead zone around SV.
void testZeroZoneDeadZoneAndFan() {
  printf("[TEST] Zero zone, dead zone, the fan holds the hold...\n");
  {   // low hold, PV a step over SV: the heater is off until PV is back under SV
    thermalv3::Controller c; thermalv3::Input in;
    in.sp = 37.5f; in.pv = 37.5f; in.permit = true; in.dtS = 0.1f;
    setHold(c, 2.0f);
    in.pv = 37.56f;
    thermalv3::Output o = c.step(in);
    assert(c.zeroZone() && o.powerPct == 0.0f);
    in.pv = 37.49f; o = c.step(in);                      // below SV - 0.02 is not yet reached: still off
    assert(o.powerPct == 0.0f);
    in.pv = 37.45f; o = c.step(in);                      // under SV - 0.02: the zone ends
    assert(!c.zeroZone());
  }
  {   // a high hold never enters the zone
    thermalv3::Controller c; thermalv3::Input in;
    in.sp = 37.5f; in.pv = 37.56f; in.permit = true; in.dtS = 0.1f;
    setHold(c, 10.0f);
    c.step(in);
    assert(!c.zeroZone());
  }
  {   // the dead zone: two readings a hundredth apart, both inside it, give the same command
    auto cmdAt = [](float pv) {
      thermalv3::Controller c; thermalv3::Input in;
      in.sp = 37.5f; in.pv = pv; in.permit = true; in.dtS = 0.1f;
      setHold(c, 8.0f);
      float o = 0.0f;
      for (int i = 0; i < 20; ++i) o = c.step(in).powerPct;
      return o;
    };
    const float a = cmdAt(37.495f), b = cmdAt(37.505f);
    assert(a > 4.0f);
    assert(std::fabs(a - b) < 0.05f);
  }
  {   // the fan: while it runs the controller learns nothing and judges nothing (the hold stays where it was)
    thermalv3::Controller c; thermalv3::Input in;
    in.sp = 37.5f; in.pv = 37.3f; in.permit = true; in.dtS = 0.1f;
    for (int i = 0; i < 6000; ++i) c.step(in);
    const float h0 = c.pHold();
    in.ventFan = true;
    for (int i = 0; i < 12000; ++i) { in.pv = 36.4f + 0.0005f * (i % 200); c.step(in); }   // 20 minutes of a flat PV 1 degC under SV (a stall) with the fan on
    assert(std::fabs(c.pHold() - h0) < 1e-3f && !c.boosting() && c.stalledWindows() == 0U);
  }
  printf("  -> Pass\n");
}

// The progress judge (progress): PV stands still far from SV. After one window (two when it is nearer than PROG_FAR_C) the learning gain g of the observer is raised.
// Nothing happens near SV, while PV is heading to SV by itself, or with a door open; g falls back when the oven follows.
void testProgressJudge() {
  printf("[TEST] The progress judge answers a stall, and only a stall...\n");
  const long WIN = 330L * 10L;                                               // one window (300 s) and a margin, at 0.1 s steps
  {   // flat 1.1 degC under SV: before the first window nothing changes, after it the learning gain is raised and the oven is pumped harder
    thermalv3::Controller c; thermalv3::Input in;
    in.sp = 37.5f; in.pv = 36.4f; in.dQ = 0.1f; in.permit = true; in.capPct = 100.0f; in.dtS = 0.1f;
    setHold(c, 18.0f);
    float cmdEarly = 0.0f;
    thermalv3::Output o;
    for (long i = 0; i < 2400; ++i) cmdEarly = c.step(in).powerPct;           // 4 minutes
    assert(!c.boosting());
    for (long i = 0; i < WIN; ++i) o = c.step(in);
    assert(c.boosting() && c.stalledWindows() >= 1U && c.boost() >= thermalv3::GAIN_STEP);
    for (long i = 0; i < 2L * WIN; ++i) o = c.step(in);
    assert(c.boost() == thermalv3::GAIN_MAX);
    assert(c.pHold() > 30.0f);                                                // the observer has learned that the oven needs more (the model said it would rise, it did not)
    printf("  -> Pass: command %.0f %% before the first window, %.0f %% after three, g=%.0f, hold %.1f %%\n", cmdEarly, o.powerPct, c.boost(), c.pHold());
  }
  {   // 0.5 under SV (nearer than PROG_FAR_C): one stalled window is not enough, two are
    thermalv3::Controller c; thermalv3::Input in;
    in.sp = 37.5f; in.pv = 37.0f; in.dQ = 0.1f; in.permit = true; in.capPct = 100.0f; in.dtS = 0.1f;
    setHold(c, 18.0f);
    for (long i = 0; i < WIN + 600L; ++i) c.step(in);                        // a minute to start measuring, one window
    assert(!c.boosting() && c.stalledWindows() == 1U);
    for (long i = 0; i < WIN; ++i) c.step(in);
    assert(c.boosting() && c.stalledWindows() >= 2U);
  }
  {   // next to SV (inside the near band) nothing is ever judged
    thermalv3::Controller c; thermalv3::Input in;
    in.sp = 37.5f; in.pv = 37.45f; in.permit = true; in.capPct = 100.0f; in.dtS = 0.1f;
    for (long i = 0; i < 4L * WIN; ++i) c.step(in);
    assert(!c.boosting() && c.stalledWindows() == 0U);
  }
  {   // PV climbing so that it reaches SV by itself in a transit time and a minute: no stall
    thermalv3::Controller c; thermalv3::Input in;
    in.sp = 37.5f; in.pv = 36.5f; in.permit = true; in.capPct = 100.0f; in.dtS = 0.1f;
    for (long i = 0; i < 2L * WIN; ++i) { in.pv += 0.0000004f * 10.0f; c.step(in); }   // 0.04 degC/min is NOT heading there in 210 s ...
    // ... so make the climb brisk enough: slope x (tau + 60 s) >= the error
    thermalv3::Controller d; thermalv3::Input id;
    id.sp = 37.5f; id.pv = 36.5f; id.permit = true; id.capPct = 100.0f; id.dtS = 0.1f;
    for (long i = 0; i < 3000; ++i) { id.pv += 0.0004f; d.step(id); }          // 0.004 degC/s: 1.0 degC in 250 s
    assert(!d.boosting());
  }
  {   // a door open: no judgement (the door has its own rule)
    thermalv3::Controller c; thermalv3::Input in;
    in.sp = 37.5f; in.pv = 37.5f; in.permit = true; in.capPct = 100.0f; in.dtS = 0.1f;
    for (long i = 0; i < 6000; ++i) c.step(in);
    float pv = 37.5f;
    bool boosted = false;
    for (long i = 0; i < 2L * WIN; ++i) { pv = std::max(34.0f, pv - 0.003f); in.pv = pv; c.step(in); boosted = boosted || c.boosting(); }   // 11 minutes: inside the door latch (6 transit times)
    assert(!boosted);
  }
  {   // when the oven follows again g falls back to 1
    thermalv3::Controller c; thermalv3::Input in;
    in.sp = 37.5f; in.pv = 36.4f; in.permit = true; in.capPct = 100.0f; in.dtS = 0.1f;
    setHold(c, 18.0f);
    for (long i = 0; i < 2L * WIN; ++i) c.step(in);
    assert(c.boosting());
    for (long i = 0; i < 1500; ++i) { in.pv = std::min(37.5f, in.pv + 0.0006f); c.step(in); }
    for (long i = 0; i < 4L * WIN; ++i) c.step(in);
    assert(!c.boosting());
  }
  printf("  -> Pass\n");
}

void testSeedHold() {
  printf("[TEST] Seeded hold (restored after a restart / one-time seed of an update)...\n");
  thermalv3::Input in;
  in.pv = 37.5f; in.sp = 37.5f; in.permit = true; in.capPct = 100.0f; in.dtS = 0.1f;
  {
    thermalv3::Controller prior; thermalv3::Output o = prior.step(in);
    assert(o.powerPct > 11.0f && o.powerPct < 13.5f && prior.holdSource() == 0);          // the prior: 12 %
  }
  {
    thermalv3::Controller c; c.seedHold(43.0f, 2U); thermalv3::Output o = c.step(in);
    assert(o.powerPct > 42.0f && o.powerPct < 44.5f);                                      // the oven starts at the hold it needed
    assert(c.holdSource() == 2U && c.pHold() > 42.9f);
    c.seedHold(80.0f, 1U); assert(c.pHold() < 50.0f && c.holdSource() == 2U);              // once it has run, nothing but the observer changes hold
  }
  { thermalv3::Controller c; c.seedHold(1.0f, 1U); assert(c.pHold() >= 3.0f); c.seedHold(95.0f, 1U); assert(c.pHold() <= 90.0f); c.seedHold(NAN, 2U); assert(c.pHold() <= 90.0f); }
  { thermalv3::Controller c; c.seedHold(43.0f, 2U); c.reset(); assert(c.holdSource() == 0U && c.pHold() > 11.0f && c.pHold() < 13.0f); }
  printf("  -> Pass: seedHold only before the first live step, clamped, cleared by reset\n");
}

int main() {
  printf("=========================================\n");
  printf("RUNNING THERMAL V3 CONTROLLER UNIT TESTS\n");
  printf("=========================================\n");
  testBasicCycle();
  testDoorDetection();
  testEnergyInFlightLowerBound();
  testPlantSimulation();
  testRestartNextToSetpointKeepsPriors();
  testStartIdentifiesTheOven();
  testStartAfterAPause();
  testDoorPlungeAfterWeeksOfUptime();
  testBoundsUnderHostileInput();
  testShadowHold();
  testResumeAfterHold();
  testSensorFallback();
  testZeroZoneDeadZoneAndFan();
  testProgressJudge();
  testSeedHold();
  printf("=========================================\n");
  printf("ALL THERMAL V3 TESTS PASSED SUCCESSFULLY!\n");
  printf("=========================================\n");
  return 0;
}
