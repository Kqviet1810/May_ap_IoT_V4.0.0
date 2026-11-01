// Unit tests of what 2.7.0 added to the thermal controller (MAYAP_INDUSTRIAL_v1_0_0/thermal_v3.h):
//  1. the exhaust fan of the day profile is MEASURED (what it takes out of the box, in % of heater power) and PAID for one transit time ahead,
//  2. Anti-Stuck does not take the delay of the oven for a stall, and does act on a stall far under SV.
#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_v3.h"
#include "../MAYAP_INDUSTRIAL_v1_0_0/sensor_fine.h"
#include "thermal-sim.h"
#include <cassert>
#include <cmath>
#include <cstdio>

using namespace thermalsim;

struct FanRun { double minAir = 99.0, lastLoss = 0.0, maxBump = 0.0; };

// Settled oven, a 2.5 kW fan block of 180 s every 20 minutes for 3 hours. withInfo: the controller is told when the blocks start and end.
static FanRun fanRun(bool withInfo) {
  PlantParams p;                       // the default oven: 16 kW, fan 2.5 kW
  Plant pl(p, 37.5, 37.4); pl.settleAt(37.5);
  SensorChain ch(0.1); for (int i = 0; i < 8; ++i) ch.sample(pl.sensor());
  sensorfine::Estimator slope; slope.reset(); sensorfine::Params sp;
  thermalv3::Controller c; thermalv3::Input in; in.sp = 37.5f; in.dQ = 0.1f; in.capPct = 100; in.dtS = 0.1f; in.permit = true;
  FanRun r;
  const double tFan0 = 7200.0, tEnd = tFan0 + 3.0 * 3600.0;
  for (long i = 0; i < static_cast<long>(tEnd * 10); ++i) {
    const double t = i * 0.1;
    if (i % 20 == 0) ch.sample(pl.sensor());
    if (i % 20 == 0) slope.update(static_cast<float>(std::lround(pl.sensor() * 10.0) / 10.0), static_cast<float>(t), 0.1f, sp);
    in.pv = ch.value();
    in.slopeFine = slope.ready() ? slope.slopePerS() : NAN;
    bool fan = false; in.ventOnInS = 1.0e6f; in.ventOffInS = 1.0e6f;
    if (t >= tFan0) {
      const double ph = std::fmod(t - tFan0, 1200.0);
      fan = ph < 180.0;
      if (withInfo) { in.ventOnInS = fan ? 0.0f : static_cast<float>(1200.0 - ph); in.ventOffInS = fan ? static_cast<float>(180.0 - ph) : static_cast<float>(1200.0 - ph + 180.0); }
    }
    in.ventFan = fan && withInfo;
    const float u = c.step(in).powerPct;
    pl.step(u, fan, false);
    if (t >= tEnd - 3600.0) r.minAir = std::min(r.minAir, pl.sensor());
    r.maxBump = std::max(r.maxBump, static_cast<double>(c.fanBump()));
  }
  r.lastLoss = c.fanLoss();
  return r;
}

int main() {
  // 1. the fan level is learned, the heater pays for it ahead, and the dips of the last hour are shallower than without the day profile
  {
    const FanRun a = fanRun(false), b = fanRun(true);
    std::printf("fan: without profile lowest air %.2f; with profile lowest air %.2f, fan level %.1f %%, largest advance %.1f %%\n", a.minAir, b.minAir, b.lastLoss, b.maxBump);
    assert(a.lastLoss == 0.0);                        // nothing is learned without the profile
    assert(b.lastLoss >= 0.5 && b.maxBump >= 0.5);    // the fan level was measured and applied
    assert(b.minAir >= a.minAir + 0.10);              // the dips are at least 0.1 degC shallower
  }
  // 2. Anti-Stuck: the flat stretch of the oven's own delay is not a stall; a stall far under SV raises the hold level
  {
    thermalv3::Controller c; thermalv3::Input in; in.sp = 37.5f; in.dQ = 0.1f; in.capPct = 100; in.dtS = 0.1f; in.permit = true; in.pv = 30.0f;
    const float h0 = c.pHold();
    for (int i = 0; i < 1000; ++i) c.step(in);        // 100 s of full power on a reading that has not moved yet: the delay of the oven
    assert(c.pHold() <= h0 + 0.5f);
    for (int i = 0; i < 6000; ++i) c.step(in);        // a further 10 minutes flat, 7.5 degC under SV: a stall
    assert(c.pHold() >= h0 + 10.0f);
    std::printf("anti-stuck: hold %.1f -> %.1f %%\n", h0, c.pHold());
  }
  // 3. a missing slope (NaN) and a hostile one change nothing and break nothing
  {
    thermalv3::Controller c; thermalv3::Input in; in.sp = 37.5f; in.dQ = 0.1f; in.capPct = 100; in.dtS = 0.1f; in.permit = true; in.pv = 37.4f;
    const float vals[] = {NAN, 1.0e9f, -1.0e9f, 0.0f, 0.19f};
    for (int k = 0; k < 5; ++k) { in.slopeFine = vals[k]; for (int i = 0; i < 600; ++i) { const float u = c.step(in).powerPct; assert(u >= 0.0f && u <= 100.0f); } }
  }
  std::printf("thermal-fan: ok\n");
  return 0;
}
