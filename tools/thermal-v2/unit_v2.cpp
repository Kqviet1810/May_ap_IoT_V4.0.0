// Host unit tests of thermal_v2.h alone (no rig, no firmware): contract of the block, not tuning quality (that is the grid's job).
#include "../../MAYAP_INDUSTRIAL_v1_0_0/thermal_v2.h"
#include <cstdio>
#include <vector>
using namespace thermalv2;
static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { ++fails; std::printf("FAIL: %s  [%s]\n", msg, #c); } else std::printf("ok:   %s\n", msg); } while (0)

// minimal plant: delay + lag heater path, one air node, 0.1 degC quantisation, samples every 2 s
struct Mini {
  double Pmax = 16000, Cf = 600000, UA = 250, td = 100, tl = 40, Ta = 30, Tamb = 28;
  std::vector<double> pipe; size_t cur = 0; double heater = 0;
  Mini() { pipe.assign(static_cast<size_t>(td * 10), 0.0); }
  void step(double on /*0..1*/, double dt) {
    const double d = pipe[cur]; pipe[cur] = on * Pmax; cur = (cur + 1) % pipe.size();
    heater += (d - heater) * dt / tl;
    Ta += (heater - UA * (Ta - Tamb)) * dt / Cf;
  }
};
static float quant(double v) { return static_cast<float>(std::floor(v / 0.1 + 0.5) * 0.1); }

struct Run { float maxU = 0, minU = 1e9f, peak = -1e9f, last = 0; bool nan = false; };
static Run simulate(Controller &c, Mini &p, double hours, float sp, float cap, double spStepAt = -1, float spAfter = 0, bool permit = true) {
  Run r; double acc = 0; Input in; in.sp = sp; in.capPct = cap; in.dtS = 0.1f; in.permit = permit; float u = 0;
  const int n = static_cast<int>(hours * 36000);
  for (int k = 0; k < n; ++k) {
    if (spStepAt >= 0 && k == static_cast<int>(spStepAt * 10)) in.sp = spAfter;
    in.pv = quant(p.Ta); in.newSample = (k % 20) == 0;
    // delivered = what the burst scheduler would give: duty u over a 3 s window
    in.deliveredPct = ((k % 30) < static_cast<int>(u * 0.3f + 0.5f)) ? 100.0f : 0.0f;
    const Output o = c.step(in); u = o.powerPct;
    if (!std::isfinite(u)) r.nan = true;
    r.maxU = std::fmax(r.maxU, u); r.minU = std::fmin(r.minU, u); r.peak = std::fmax(r.peak, static_cast<float>(p.Ta)); r.last = u;
    p.step(in.deliveredPct > 0 ? 1.0 : 0.0, 0.1); (void)acc;
  }
  return r;
}

int main() {
  { Controller c; Input in; in.pv = 33.0f; in.sp = 37.5f; in.permit = false; in.newSample = true; Output o = c.step(in);
    CHECK(o.powerPct == 0.0f && o.state == StFrozen, "no permit: no power, state Frozen"); }
  { Controller c; Input in; in.pv = NAN; in.sp = 37.5f; in.permit = true; Output o = c.step(in);
    CHECK(o.powerPct == 0.0f, "NaN sensor value: no power"); }
  { Controller c; Input in; in.pv = 33.0f; in.sp = NAN; in.permit = true; in.newSample = true; Output o = c.step(in);
    CHECK(o.powerPct == 0.0f, "NaN setpoint: no power"); }
  { Controller c; Input in; in.pv = 33.0f; in.sp = 37.5f; in.permit = true; in.capPct = 0.0f; in.newSample = true; Output o = c.step(in);
    CHECK(o.powerPct == 0.0f, "cap 0: no power"); }
  { Controller c; Mini p; Run r = simulate(c, p, 3.0, 37.5f, 100.0f);
    CHECK(!r.nan && r.minU >= 0.0f && r.maxU <= 100.0f, "cold start 3 h: output stays in [0, 100] and finite");
    CHECK(std::fabs(p.Ta - 37.5) < 0.6, "cold start 3 h: ends within 0.6 degC of SP (quantised sensor, delay + lag plant)");
    CHECK(r.peak < 38.5f, "cold start 3 h: peak below 38.5 degC"); }
  { Controller c; Mini p; Run r = simulate(c, p, 2.0, 37.5f, 40.0f);
    CHECK(r.maxU <= 40.0f + 1e-3f, "cap 40: output never exceeds the cap"); }
  { Controller c; Mini p; Run r = simulate(c, p, 4.0, 37.0f, 100.0f, 2.5 * 3600, 37.5f);
    CHECK(!r.nan && std::fabs(p.Ta - 37.5) < 0.6, "SP step +0.5 after 2.5 h: follows within 0.6 degC"); }
  { Controller a, b; Mini pa, pb; simulate(a, pa, 1.0, 37.5f, 100.0f); simulate(b, pb, 1.0, 37.5f, 100.0f);
    CHECK(pa.Ta == pb.Ta, "deterministic: same input, same trajectory"); }
  { Controller c; Mini p; simulate(c, p, 0.5, 37.5f, 100.0f); c.reset();
    Input in; in.pv = 33.0f; in.sp = 37.5f; in.permit = true; in.newSample = true; const Output o = c.step(in);
    CHECK(c.integral() == 0.0f && std::isfinite(o.powerPct), "reset clears the state"); }
  { Controller c; Mini p; simulate(c, p, 1.0, 37.5f, 100.0f);
    Input in; in.sp = 37.5f; in.permit = false; in.pv = quant(p.Ta); in.dtS = 0.1f; float last = 1;
    for (int k = 0; k < 3000; ++k) { in.newSample = (k % 20) == 0; last = c.step(in).powerPct; }
    CHECK(last == 0.0f, "permit lost for 300 s: output stays 0 (no wind-up into a command)"); }
  std::printf("%s (%d failure(s))\n", fails ? "UNIT FAILED" : "UNIT PASSED", fails);
  return fails ? 1 : 0;
}
