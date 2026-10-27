// Host tests of thermal_v3.h: the contract of the block (safety, start-up, measured model, stopping) on small closed loops.
// The grid over many ovens and disturbances is tools/thermal-v3/benchmark_v3.py; a pass here is not proof for a real oven.
#include <cmath>
#include <cstdio>
#include <vector>
#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_v3.h"
#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_v3_adapter.h"

namespace {

int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { ++g_fail; std::printf("FAIL: %s  [%s]\n", msg, #cond); } else std::printf("ok:   %s\n", msg); } while (0)

// heater (delay + lag) -> air (loss to the room) <-> egg node; the sensor steps of 0.1 degC every 2 s
struct Oven {
  double Pmax, Cf, Ce, hA, UA, td, tl, Tamb;
  double Ta, Te, heater, sens;
  std::vector<double> pipe;
  size_t pi;
  Oven(double pmax, double cf, double ce, double ha, double ua, double d, double l, double amb, double t0)
      : Pmax(pmax), Cf(cf), Ce(ce), hA(ha), UA(ua), td(d), tl(l), Tamb(amb), Ta(t0), Te(t0), heater(0), sens(t0), pipe(static_cast<size_t>(d / 0.1) + 1, 0.0), pi(0) {}
  void step(double pct, double dt = 0.1) {
    const double pw = pct / 100.0 * Pmax, d = pipe[pi];
    pipe[pi] = pw; pi = (pi + 1) % pipe.size();
    heater += (d - heater) * dt / tl;
    const double qe = hA * (Ta - Te);
    Ta += (heater - UA * (Ta - Tamb) - qe) / Cf * dt;
    Te += qe / Ce * dt;
  }
  float reading() const { return static_cast<float>(std::floor(Ta * 10.0 + 0.5) / 10.0); }
};

struct Run { double uMaxNear = 0, powerAt1 = -1, peak = -1, finalMin = 1e9, finalMax = -1e9, powerAtSp = -1; bool knownAtEnd = false; double firstPowerS = -1, probeMax = 0; float kEst = 0; };

Run simulate(Oven &o, ThermalV3Slot &slot, double hours, float sp, float cap = 100.0f, bool permit = true) {
  Run r; double u = 0; uint32_t ms = 0; float pv = o.reading();
  const long n = static_cast<long>(hours * 36000);
  for (long k = 0; k < n; ++k) {
    o.step(u);
    if (k % 20 == 0) pv = o.reading();
    ms += 100;
    ThermalV3SlotIn in; in.nowMs = ms; in.pv = pv; in.sp = sp; in.newSample = (k % 20 == 0); in.permit = permit; in.capPct = cap;
    u = slot.step(in);
    r.peak = std::max(r.peak, o.Ta);
    if (u > 0.0 && r.firstPowerS < 0) r.firstPowerS = k * 0.1;
    if (slot.controller().stage() != 3) r.probeMax = std::max(r.probeMax, u);
    if (r.powerAt1 < 0 && pv >= sp - 1.0f) r.powerAt1 = u;
    if (k < 18000 && pv >= sp - 0.5f) r.uMaxNear = std::max(r.uMaxNear, u);
    if (r.powerAtSp < 0 && pv >= sp - 0.05f) r.powerAtSp = u;
    if (k >= n - 36000) { r.finalMin = std::min(r.finalMin, o.Ta); r.finalMax = std::max(r.finalMax, o.Ta); }
  }
  r.knownAtEnd = slot.controller().identified();
  r.kEst = slot.controller().kAir();
  return r;
}

void testSafety() {
  thermalv3::Controller c; thermalv3::Input in; in.pv = 33.0f; in.sp = 37.5f; in.capPct = 100.0f;
  thermalv3::Output o = c.step(in);
  CHECK(o.powerPct == 0.0f && o.state == thermalv3::StFrozen, "no permit: no power, state Frozen");
  in.permit = true; in.pv = NAN; o = c.step(in);
  CHECK(o.powerPct == 0.0f, "NaN sensor value: no power");
  in.pv = 33.0f; in.sp = NAN; o = c.step(in);
  CHECK(o.powerPct == 0.0f, "NaN setpoint: no power");
  in.sp = 37.5f; in.capPct = 0.0f; o = c.step(in);
  CHECK(o.powerPct == 0.0f, "cap 0: no power");
  in.capPct = 100.0f; in.permit = false; in.shadow = true; o = c.step(in);
  CHECK(o.powerPct == 0.0f && o.state == thermalv3::StFrozen, "the vent line holds the heater (shadow): no power");
  in.shadow = false; in.permit = true; in.dtS = NAN; o = c.step(in);
  CHECK(std::isfinite(o.powerPct) && o.powerPct >= 0.0f, "NaN step time: output stays finite");
}

void testStartUp() {
  Oven o(16000, 500000, 1900000, 1300, 250, 100, 40, 28, 33.0);
  ThermalV3Slot slot; const Run r = simulate(o, slot, 4.0, 37.5f);
  CHECK(r.firstPowerS >= 89.0, "cold start: the heater stays off for the 90 s look");
  CHECK(r.probeMax <= 25.0 + 1e-3, "cold start: before the law runs the power never exceeds the 25 % probe pulse");
  CHECK(r.knownAtEnd, "the probe measured the gain of the oven (model known)");
  const double kTrue = (16000.0 / 100.0) / 500000.0;        // degC/s per % of the air node
  CHECK(r.kEst > 0.4 * kTrue && r.kEst < 2.5 * kTrue, "the measured gain is within a factor of 2.5 of the air node's");
  CHECK(r.peak < 37.5 + 0.45, "cold start: the first climb overshoots by less than 0.45 degC");
  CHECK(r.finalMax - r.finalMin < 0.4 && std::fabs(0.5 * (r.finalMax + r.finalMin) - 37.5) < 0.15, "the last hour holds within 0.2 of SP (span < 0.4)");
}

void testCap() {
  Oven o(16000, 500000, 1900000, 1300, 250, 100, 40, 28, 33.0);
  ThermalV3Slot slot; double mx = 0; uint32_t ms = 0; double u = 0; float pv = o.reading();
  for (long k = 0; k < 36000; ++k) {
    o.step(u); if (k % 20 == 0) pv = o.reading(); ms += 100;
    ThermalV3SlotIn in; in.nowMs = ms; in.pv = pv; in.sp = 37.5f; in.permit = true; in.capPct = 12.0f;
    u = slot.step(in); mx = std::max(mx, u);
  }
  CHECK(mx <= 12.0 + 1e-3, "a heater cap of 12 % is never exceeded");
}

void testNoFlatPowerToSp() {
  // the owner's 24 Oct log: 20 % held from 36.0 to 37.5 degC and the air went on to 37.8.  The power must have come down well before SP.
  Oven o(20000, 1150000, 3450000, 1566, 50, 73, 52, 32.4, 33.0);
  ThermalV3Slot slot; const Run r = simulate(o, slot, 3.0, 37.5f);
  CHECK(r.uMaxNear < 35.0, "owner's oven: within 0.5 degC of SP the first climb never asks more than 35 % (it was a flat 20 % to SP and +0.3 over)");
  CHECK(r.peak < 37.5 + 0.25, "owner's oven: overshoot below 0.25 degC");
}

void testAboveSpClimbing() {
  // an air above SP that keeps climbing (4 mK/s) gets no power at all
  ThermalV3Slot slot; uint32_t ms = 0; double u = 0, ta = 37.9, mx = 0;
  for (long k = 0; k < 6000; ++k) {
    ta += 0.0004; ms += 100;
    ThermalV3SlotIn in; in.nowMs = ms; in.pv = static_cast<float>(std::floor(ta * 10.0 + 0.5) / 10.0); in.sp = 37.5f; in.permit = true; in.capPct = 100.0f; u = slot.step(in);
    if (k >= 3000) mx = std::max(mx, u);
  }
  CHECK(mx < 0.5, "above SP and still climbing: no power");
}

void testModelKept() {
  Oven o(16000, 500000, 1900000, 1300, 250, 100, 40, 28, 33.0);
  ThermalV3Slot slot; simulate(o, slot, 2.0, 37.5f);
  const thermalv3::Model m = slot.controller().exportModel();
  CHECK(m.known && m.k > 0.0f, "the model can be exported after a run");
  thermalv3::Controller c;
  CHECK(c.importModel(m), "a valid model is accepted");
  thermalv3::Model bad = m; bad.k = -1.0f;
  CHECK(!c.importModel(bad), "an impossible model is refused");
  // a permit gap keeps the model and does not repeat the start-up
  ThermalV3SlotIn in; in.nowMs = 1000000; in.pv = 37.4f; in.sp = 37.5f; in.permit = false; in.capPct = 100.0f; slot.step(in);
  CHECK(slot.controller().identified(), "a permit gap keeps the measured model");
}

void testDeterministic() {
  Oven a(16000, 500000, 1900000, 1300, 250, 100, 40, 28, 33.0), b(16000, 500000, 1900000, 1300, 250, 100, 40, 28, 33.0);
  ThermalV3Slot sa, sb; simulate(a, sa, 1.0, 37.5f); simulate(b, sb, 1.0, 37.5f);
  CHECK(a.Ta == b.Ta, "same input, same trajectory");
}

void testNoPrediction() {
  Oven o(16000, 500000, 1900000, 1300, 250, 100, 40, 28, 33.0);
  ThermalV3Slot slot; simulate(o, slot, 0.5, 37.5f);
  CHECK(slot.controller().predicted() == o.reading() || std::fabs(slot.controller().predicted() - o.reading()) < 0.11, "the read-out 'predicted' is the measured PV: no prediction exists");
}

void testLowLossOven() {
  // an almost lossless oven (hold ~2 %) must not sit above SP
  Oven o(16000, 1200000, 1900000, 1000, 31, 150, 50, 28, 33.0);
  ThermalV3Slot slot; const Run r = simulate(o, slot, 4.0, 37.5f);
  CHECK(r.finalMax - 37.5 < 0.2 && r.finalMin > 37.5 - 0.2, "a low-loss oven holds within 0.2 of SP");
}

}  // namespace

int main() {
  testSafety();
  testStartUp();
  testCap();
  testNoFlatPowerToSp();
  testAboveSpClimbing();
  testModelKept();
  testDeterministic();
  testNoPrediction();
  testLowLossOven();
  std::printf(g_fail ? "THERMAL V3 TESTS FAILED (%d)\n" : "THERMAL V3 TESTS PASSED\n", g_fail);
  return g_fail ? 1 : 0;
}
