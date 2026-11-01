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


// Runs a plant with the controller for `seconds`; returns the highest temperature and leaves the controller in its state.
static double drive(thermalv3::Controller &ctl, TestPlant &plant, thermalv3::Input &in, double seconds, double *lowest = nullptr) {
  double peak = 0.0, low = 1e9;
  long sensorTick = 0;
  static double pvq = 0.0;                                                  // the 0.1 degC sensor, read every 2 s
  for (long i = 0; i < static_cast<long>(seconds * 10.0); ++i) {
    if (sensorTick++ % 20 == 0) pvq = std::round(plant.Ta * 10.0) / 10.0;
    in.pv = static_cast<float>(pvq);
    const double u = ctl.step(in).powerPct;
    plant.step(u);
    peak = std::max(peak, plant.Ta);
    low = std::min(low, plant.Ta);
  }
  if (lowest) *lowest = low;
  return peak;
}

void testBasicCycle() {
  printf("[TEST] Basic Cycle & State Transitions...\n");
  thermalv3::Controller ctl;
  thermalv3::Input in;
  in.pv = 30.0f; in.sp = 37.5f; in.permit = true; in.capPct = 100.0f; in.dtS = 0.1f;
  thermalv3::Output out = ctl.step(in);
  assert(out.powerPct > 90.0f);                                              // an oven far under SV gets the whole heater
  assert(out.state == thermalv3::StClimb);
  in.permit = false;
  out = ctl.step(in);
  assert(out.powerPct == 0.0f && out.state == thermalv3::StFrozen);
  in.permit = true; in.pv = NAN;
  out = ctl.step(in);
  assert(out.powerPct == 0.0f && out.state == thermalv3::StFrozen);          // no reading, no heat
  in.pv = 37.5f; in.capPct = 40.0f;
  out = ctl.step(in);
  assert(out.powerPct <= 40.0f + 1e-3f);                                     // the ceiling holds
  printf("  -> Pass\n");
}

void testDoorDetection() {
  printf("[TEST] Door Open Detection...\n");
  thermalv3::Controller ctl;
  thermalv3::Input in;
  in.sp = 37.5f; in.pv = 37.5f; in.permit = true; in.capPct = 100.0f; in.dtS = 0.1f;
  for (int i = 0; i < 3000; ++i) ctl.step(in);                               // five minutes at SV: the load settles to about nothing
  const float before = ctl.load();
  float pv = 37.5f;
  bool door = false; float cmd = 0.0f;
  for (int i = 0; i < 600; ++i) { pv -= 0.002f; in.pv = pv; cmd = ctl.step(in).powerPct; door = door || ctl.doorDetected(); }   // 60 s of -0.02 degC/s
  assert(door);
  assert(cmd <= ctl.load() + 6.0f + 1e-3f);                                   // the heater is held at the load + 6 % while the door is open
  printf("  -> Pass (load %.1f %%, command %.1f %%)\n", before, cmd);
}

void testPlantSimulation() {
  printf("[TEST] Full Plant Simulation from 30.0C to 37.5C (2 hours)...\n");
  TestPlant plant;
  thermalv3::Controller ctl;
  thermalv3::Input in;
  in.sp = 37.5f; in.permit = true; in.capPct = 100.0f; in.dtS = 0.1f;
  double low = 0.0;
  const double peak = drive(ctl, plant, in, 2.0 * 3600.0, &low);
  printf("  -> peak %.2f, end %.2f, identified %d tau %.0f s kAir %.2e\n", peak, plant.Ta, ctl.identified() ? 1 : 0, ctl.tauTransit(), ctl.kAir());
  assert(peak < 38.2);
  assert(std::fabs(plant.Ta - 37.5) < 0.3);
  assert(ctl.identified());
  printf("  -> Pass\n");
}

// The load the heater has to carry changes by a lot and nobody says so: the oven holds 37.5 with a few percent, then cold eggs soak up 3.5 kW for
// good. A reading that stands under SV with nothing on its way must make the load climb until it carries them (the batch of 01/11: 56 % at 36.0
// for seven minutes and 62-95 % afterwards, under the old rule the heater sat at 56 %).
void testFindsALoad() {
  printf("[TEST] A load nobody announced is found by the integral...\n");
  TestPlant plant; plant.Ta = 37.5;
  thermalv3::Controller ctl;
  thermalv3::Input in;
  in.sp = 37.5f; in.permit = true; in.capPct = 100.0f; in.dtS = 0.1f;
  ctl.importLoad(static_cast<float>(plant.UA * (37.5 - plant.Tamb) / plant.Pmax * 100.0), true);
  drive(ctl, plant, in, 1800.0);
  const float before = ctl.load();
  plant.sinkW = 3500.0;                                                       // 22 % of the heater (a slower fall than a door makes)
  double low = 0.0;
  drive(ctl, plant, in, 1200.0, &low);
  printf("  -> load %.1f -> %.1f %%, lowest %.2f, after 20 min %.2f\n", before, ctl.load(), low, plant.Ta);
  assert(ctl.load() > before + 12.0f);
  assert(low > 35.0);
  drive(ctl, plant, in, 1800.0);
  assert(std::fabs(plant.Ta - 37.5) < 0.25);
  printf("  -> Pass\n");
}

// Next to SV the heater is braked by the MEASURED rise: the reading climbs 0.28 degC/min with the load at 60 %, as on 01/11 at 05:23.
void testBrakesOnTheMeasuredRise() {
  printf("[TEST] A reading that is still rising next to SV brakes the heater...\n");
  thermalv3::Controller ctl;
  ctl.importOven(1.1e-4f, 113.0f);
  ctl.importLoad(62.0f, true);
  thermalv3::Input in;
  in.sp = 37.5f; in.permit = true; in.capPct = 100.0f; in.dtS = 0.1f;
  float pv = 36.7f, cmd = 0.0f, atSv = 100.0f;
  for (int s = 0; s < 3 * 60 * 10; ++s) {                                     // 3 minutes, +0.0047 degC/s
    pv += 0.0047f / 10.0f;
    in.pv = std::round(pv * 10.0f) / 10.0f;
    cmd = ctl.step(in).powerPct;
    if (in.pv >= 37.4f && atSv > 99.0f) atSv = cmd;
  }
  printf("  -> pv %.2f command %.1f %% (load 62 %%), command when the reading first showed 37.4: %.1f %%\n", in.pv, cmd, atSv);
  assert(atSv < 45.0f);
  printf("  -> Pass\n");
}

// The permit is lost while the load is high: nothing is learned and nothing moves; the heater comes back at the load.
void testShadowHold() {
  printf("[TEST] Vent-line hold: the load is kept, the heater is 0 %% while it lasts...\n");
  thermalv3::Controller ctl;
  thermalv3::Input in;
  in.sp = 37.5f; in.pv = 37.5f; in.permit = true; in.capPct = 100.0f; in.dtS = 0.1f;
  ctl.importLoad(30.0f, true);
  for (int i = 0; i < 600; ++i) ctl.step(in);
  const float before = ctl.load();
  in.permit = false; in.shadow = true; in.pv = 37.9f;
  float cmd = 1.0f;
  for (int i = 0; i < 3000; ++i) cmd = ctl.step(in).powerPct;
  assert(cmd == 0.0f);
  assert(std::fabs(ctl.load() - before) < 0.01f);
  in.shadow = false; in.permit = true; in.pv = 37.5f;
  for (int i = 0; i < 300; ++i) cmd = ctl.step(in).powerPct;
  assert(cmd > before - 8.0f && cmd < before + 8.0f);
  printf("  -> Pass (load %.1f %%, back at %.1f %%)\n", before, cmd);
}

// A controller that restarts next to SV has seen no start: it must not take the first 0.15 degC it sees for the transit time of the oven.
void testRestartNextToSetpoint() {
  printf("[TEST] Restart next to SP: no false identification...\n");
  thermalv3::Controller ctl;
  const float tauPrior = ctl.tauTransit(), kPrior = ctl.kAir();
  thermalv3::Input in;
  in.sp = 37.5f; in.pv = 37.4f; in.permit = true; in.capPct = 100.0f; in.dtS = 0.1f;
  for (int i = 0; i < 18000; ++i) { if (i % 3000 == 0) in.pv += 0.1f; ctl.step(in); }
  assert(!ctl.identified());
  assert(ctl.tauTransit() == tauPrior && ctl.kAir() == kPrior);
  printf("  -> Pass\n");
}

void testStartIdentifiesTheOven() {
  printf("[TEST] A real start identifies delay and gain inside their limits...\n");
  TestPlant plant; plant.Ta = 30.0;
  thermalv3::Controller ctl;
  thermalv3::Input in;
  in.sp = 37.5f; in.permit = true; in.capPct = 100.0f; in.dtS = 0.1f;
  drive(ctl, plant, in, 1800.0);
  printf("  -> identified=%d tau=%.0f s kAir=%.2e\n", ctl.identified() ? 1 : 0, ctl.tauTransit(), ctl.kAir());
  assert(ctl.identified());
  assert(ctl.tauTransit() >= 90.0f && ctl.tauTransit() <= 220.0f);
  assert(ctl.kAir() >= thermalv3::KAIR_MIN && ctl.kAir() <= thermalv3::KAIR_MAX);
  printf("  -> Pass\n");
}

// The day profile of the exhaust fans is known ahead of time: the extra heat the running block takes is learned and the heater is raised
// one transit time before the next block starts and withdrawn before it ends.
void testFanBump() {
  printf("[TEST] Scheduled exhaust fans: the bump is learned and applied before the block...\n");
  TestPlant plant; plant.Ta = 37.5;
  thermalv3::Controller ctl;
  thermalv3::Input in;
  in.sp = 37.5f; in.permit = true; in.capPct = 100.0f; in.dtS = 0.1f;
  ctl.importOven(1.1e-4f, 100.0f);
  ctl.importLoad(static_cast<float>(plant.UA * (37.5 - plant.Tamb) / plant.Pmax * 100.0), true);
  const double period = 2400.0, block = 300.0;
  double t = 0.0;
  static double pvq = 37.5;
  for (int cycle = 0; cycle < 6; ++cycle) {
    for (long i = 0; i < static_cast<long>(period * 10.0); ++i, t += 0.1) {
      const double ph = std::fmod(t, period);
      const bool inBlock = ph >= period - block;
      in.ventOnInS = inBlock ? 0.0f : static_cast<float>(period - block - ph);
      in.ventOffInS = inBlock ? static_cast<float>(period - ph) : in.ventOnInS + static_cast<float>(block);
      in.ventFan = inBlock;
      plant.sinkW = inBlock ? 3000.0 : 0.0;
      if (i % 20 == 0) pvq = std::round(plant.Ta * 10.0) / 10.0;
      in.pv = static_cast<float>(pvq);
      plant.step(ctl.step(in).powerPct);
    }
  }
  printf("  -> fan bump learned %.1f %% (the block takes %.1f %% of the heater)\n", ctl.fanBump(), 3000.0 / plant.Pmax * 100.0);
  assert(ctl.fanBump() > 3.0f && ctl.fanBump() <= thermalv3::FAN_BUMP_MAX_PCT);
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


// Whatever the temperature does, the load, the fan bump and the command stay inside their limits and finite.
void testBoundsUnderHostileInput() {
  printf("[TEST] Load, fan bump and command stay bounded under hostile input...\n");
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
    in.ventOnInS = static_cast<float>((x >> 8) % 400U) - 20.0f;
    in.ventOffInS = in.ventOnInS + 300.0f;
    in.shadow = (step / 5000) % 7 == 3;
    in.permit = !in.shadow && (step / 7000) % 11 != 5;
    if (step % 9001 == 0) in.pv = NAN;                                                // a lost sample now and then
    const thermalv3::Output out = ctl.step(in);
    assert(std::isfinite(out.powerPct) && out.powerPct >= 0.0f && out.powerPct <= 80.0f);
    assert(ctl.load() >= 0.0f && ctl.load() <= 80.0f);
    assert(ctl.fanBump() >= 0.0f && ctl.fanBump() <= thermalv3::FAN_BUMP_MAX_PCT);
    assert(std::isfinite(ctl.kAir()) && ctl.kAir() >= thermalv3::KAIR_MIN && ctl.kAir() <= thermalv3::KAIR_MAX);
    assert(ctl.tauTransit() >= thermalv3::TAU_MIN_S && ctl.tauTransit() <= thermalv3::TAU_MAX_S);
  }
  printf("  -> Pass\n");
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


int main() {
  printf("=========================================\n");
  printf("RUNNING THERMAL V3 CONTROLLER UNIT TESTS\n");
  printf("=========================================\n");
  testBasicCycle();
  testDoorDetection();
  testPlantSimulation();
  testFindsALoad();
  testBrakesOnTheMeasuredRise();
  testShadowHold();
  testRestartNextToSetpoint();
  testStartIdentifiesTheOven();
  testFanBump();
  testDoorPlungeAfterWeeksOfUptime();
  testBoundsUnderHostileInput();
  testSensorFallback();
  printf("=========================================\n");
  printf("ALL THERMAL V3 TESTS PASSED SUCCESSFULLY!\n");
  printf("=========================================\n");
  return 0;
}
