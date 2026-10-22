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
static double g_q = 0.1;                                      // sensor step of the test
static float quant(double v) { return static_cast<float>(std::floor(v / g_q + 0.5) * g_q); }

struct Run { float maxU = 0, minU = 1e9f, peak = -1e9f, last = 0; bool nan = false; };
static Run simulate(Controller &c, Mini &p, double hours, float sp, float cap, double spStepAt = -1, float spAfter = 0, bool permit = true) {
  Run r; double acc = 0; Input in; in.sp = sp; in.capPct = cap; in.dtS = 0.1f; in.permit = permit; in.dQ = static_cast<float>(g_q); float u = 0;
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
  // ---- findings of the independent review (each one failed before the fix) ----
  { Controller c; Mini p; simulate(c, p, 0.4, 37.5f, 100.0f);          // valid samples first, then the sensor goes NaN for 10 minutes
    Input in; in.sp = 37.5f; in.permit = true; in.pv = NAN; in.dtS = 0.1f; in.capPct = 100.0f; float last = 1.0f; bool nan = false;
    for (int k = 0; k < 6000; ++k) { in.newSample = (k % 20) == 0; const Output o = c.step(in); last = o.powerPct; if (!std::isfinite(last)) nan = true; }
    CHECK(!nan && last == 0.0f, "sensor NaN for 10 min: no power after the freeze time, never NaN");
    in.pv = quant(p.Ta); for (int k = 0; k < 3000; ++k) { in.newSample = (k % 20) == 0; const Output o = c.step(in); if (!std::isfinite(o.powerPct)) nan = true; }
    CHECK(!nan, "sensor back after the dropout: output finite"); }
  { Controller c; Input in; in.sp = 37.5f; in.permit = true; in.dtS = 0.1f; in.capPct = 100.0f; bool bad = false;   // no sample for 100 s at the start, then valid data
    in.pv = NAN; for (int k = 0; k < 1000; ++k) { in.newSample = (k % 20) == 0; if (!std::isfinite(c.step(in).powerPct)) bad = true; }
    in.pv = 33.0f; for (int k = 0; k < 20000; ++k) { in.newSample = (k % 20) == 0; const Output o = c.step(in); if (!std::isfinite(o.powerPct) || o.powerPct < 0 || o.powerPct > 100.0f) bad = true; }
    CHECK(!bad, "no sample at the start: learner stays finite (was a permanent NaN)"); }
  { Controller c; Input in; in.sp = 37.5f; in.permit = true; in.dtS = 0.1f; in.capPct = 100.0f; in.pv = 33.0f; bool full = false;   // NaN setpoint exactly when the look ends
    for (int k = 0; k < 2000; ++k) { in.newSample = (k % 20) == 0; in.sp = (k >= 900 && k < 1100) ? NAN : 37.5f; const Output o = c.step(in); if (k >= 900 && k < 1100 && o.powerPct > 0.0f) full = true; }
    CHECK(!full, "NaN setpoint around the end of the look: no heat while it lasts");
    bool over = false; for (int k = 2000; k < 6000; ++k) { in.newSample = (k % 20) == 0; if (c.step(in).powerPct > 45.0f) over = true; }
    CHECK(!over, "identification power stays at the safe level after a NaN setpoint (was 100 %)"); }
  { Controller c; Input in; in.sp = 37.5f; in.permit = true; in.pv = 33.0f; in.capPct = 100.0f; bool bad = false;       // NaN / huge dtS
    for (int k = 0; k < 300; ++k) { in.dtS = 0.1f; in.newSample = (k % 20) == 0; c.step(in); }
    in.dtS = NAN; c.step(in); in.dtS = 1e30f; c.step(in); in.dtS = -5.0f; c.step(in);
    const float t0 = c.clockS(); in.dtS = 0.1f; for (int k = 0; k < 3000; ++k) { in.newSample = (k % 20) == 0; if (!std::isfinite(c.step(in).powerPct)) bad = true; }
    CHECK(!bad && c.clockS() > t0 + 250.0f, "NaN / negative / huge dtS: the clock keeps running afterwards (was frozen for good)"); }
  { Controller c; Input in; in.sp = 37.5f; in.permit = true; in.capPct = 100.0f; in.dtS = 0.1f; bool bad = false;          // absurd pv
    in.pv = 33.0f; for (int k = 0; k < 600; ++k) { in.newSample = (k % 20) == 0; c.step(in); }
    in.pv = 3e38f; for (int k = 0; k < 400; ++k) { in.newSample = (k % 20) == 0; const Output o = c.step(in); if (!std::isfinite(o.powerPct) || o.powerPct != 0.0f) bad = true; }
    in.pv = 33.0f; for (int k = 0; k < 3000; ++k) { in.newSample = (k % 20) == 0; if (!std::isfinite(c.step(in).powerPct)) bad = true; }
    CHECK(!bad, "pv = 3e38 for 40 s: no power, no NaN afterwards"); }
  { Controller c; Mini p; Run r = simulate(c, p, 0.35, 37.5f, 100.0f);                 // SP lowered 12 degC below the air in the middle of the identification climb
    Input in; in.sp = 25.0f; in.permit = true; in.pv = quant(p.Ta); in.capPct = 100.0f; in.dtS = 0.1f; float mx = 0;
    for (int k = 0; k < 4000; ++k) { in.newSample = (k % 20) == 0; mx = std::fmax(mx, c.step(in).powerPct); }
    CHECK(mx < 5.0f, "SP lowered far below the air during identification: no probe power left over"); (void)r; }
  { Controller c; Mini p; simulate(c, p, 0.9, 37.5f, 100.0f);
    CHECK(c.batchesDone() == 2, "both batch fits run (the second one 1500 s after the first power)"); }
  { Controller c; Mini p; simulate(c, p, 1.0, 37.5f, 100.0f); c.reset();
    CHECK(c.batchesDone() == 0 && c.clockS() == 0.0f && std::fabs(c.kEst() - A0 * 1e-5f) < 1e-9f && c.integral() == 0.0f, "reset restores the priors and the clock"); }
  { Controller c; Mini p; Run total; bool bad = false; unsigned rng = 12345;               // fuzz: permit toggling, cap changes, SP steps, dropouts
    Input in; in.sp = 37.5f; in.capPct = 100.0f; in.dtS = 0.1f; in.permit = true; float u = 0;
    for (int k = 0; k < 400000; ++k) {
      rng = rng * 1664525u + 1013904223u; const unsigned r = rng >> 8;
      if (k % 5000 == 0) { in.permit = (r % 7) != 0; in.capPct = 20.0f + (r % 81); in.sp = 36.0f + (r % 30) / 10.0f; }
      in.pv = (r % 50000 == 0) ? NAN : quant(p.Ta); in.newSample = (k % 20) == 0;
      in.deliveredPct = ((k % 30) < static_cast<int>(u * 0.3f + 0.5f)) ? 100.0f : 0.0f;
      const Output o = c.step(in); u = o.powerPct;
      if (!std::isfinite(u) || u < 0.0f || u > in.capPct + 1e-3f || (!in.permit && u != 0.0f)) bad = true;
      p.step(in.deliveredPct > 0 ? 1.0 : 0.0, 0.1);
    }
    (void)total; CHECK(!bad, "fuzz 11 h: output always finite, within [0, cap], zero without permit"); }
  { Controller c; Mini p; Input in; in.sp = 37.5f; in.capPct = 100.0f; in.dtS = 0.1f; in.permit = true; float u = 0; bool frozen = false, suspected = false, leaked = false; float held = 0;
    for (int k = 0; k < 36000 * 2; ++k) {                                                      // sensor freezes LOW 10 min into the climb: the response watchdog must withdraw the request
      if (!frozen && k == 6000) { frozen = true; held = quant(p.Ta); }
      in.pv = frozen ? held : quant(p.Ta); in.newSample = (k % 20) == 0;
      in.deliveredPct = ((k % 30) < static_cast<int>(u * 0.3f + 0.5f)) ? 100.0f : 0.0f;
      const Output o = c.step(in); u = o.powerPct;
      if (frozen && c.responseSuspect()) suspected = true;
      if (suspected && u > 0.0f) leaked = true;
      p.step(in.deliveredPct > 0 ? 1.0 : 0.0, 0.1);
    }
    CHECK(suspected && !leaked, "frozen low sensor while climbing: response watchdog trips and keeps the request at 0");
    CHECK(p.Ta < 40.0, "frozen low sensor while climbing: the air stays below 40 degC"); }
  g_q = 0.01;                                                                                  // fine sensor: the egg observer is active
  { Controller c; Mini p; Run r = simulate(c, p, 3.0, 37.5f, 100.0f);
    CHECK(!r.nan && r.minU >= 0.0f && r.maxU <= 100.0f && r.peak < 38.5f, "fine sensor, cold start 3 h: finite, in [0, 100], peak below 38.5 degC");
    CHECK(std::fabs(p.Ta - 37.5) < 0.4, "fine sensor, cold start 3 h: ends within 0.4 degC of SP"); }
  { Controller c; Mini p; bool bad = false; unsigned rng = 777; Input in; in.sp = 37.5f; in.capPct = 100.0f; in.dtS = 0.1f; in.permit = true; in.dQ = 0.01f; float u = 0;
    for (int k = 0; k < 400000; ++k) {                                                          // tray / door style air steps and sensor noise on a fine sensor
      rng = rng * 1664525u + 1013904223u; const unsigned r = rng >> 8;
      if (k > 60000 && k % 40000 == 0) p.Ta -= 0.5 + (r % 30) / 10.0;
      in.pv = quant(p.Ta + ((r % 100) - 50) * 0.0006); in.newSample = (k % 20) == 0;
      in.deliveredPct = ((k % 30) < static_cast<int>(u * 0.3f + 0.5f)) ? 100.0f : 0.0f;
      const Output o = c.step(in); u = o.powerPct;
      if (!std::isfinite(u) || u < 0.0f || u > 100.0f + 1e-3f || !std::isfinite(c.teModel())) bad = true;
      p.step(in.deliveredPct > 0 ? 1.0 : 0.0, 0.1);
    }
    CHECK(!bad && p.Ta < 38.6, "fine sensor, 11 h with air steps and noise: output and egg model stay finite, no runaway"); }
  g_q = 0.1;
  std::printf("%s (%d failure(s))\n", fails ? "UNIT FAILED" : "UNIT PASSED", fails);
  return fails ? 1 : 0;
}
