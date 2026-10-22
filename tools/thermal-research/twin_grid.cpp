#include "thermal-adaptive-sim.h"
#include <cstdio>
#include <sstream>
#include <vector>
#include <algorithm>
#include <cmath>
#include <string>
#include <cstdlib>
using namespace sim;
// Digital twin of the user's oven (2.5 m x 2 m chamber, 4 x 5 kW, 4 trolleys / ~10000 eggs, 2 kW circulation motor).
// usage: twin_grid part parts [heaterW]   env: MODE=predictive|smart, SCN=<scenario digits>, STRIDE/PHASE (subset of ovens), T1START (first-batch start temperature)
// Used by tools/check_thermal_acceptance.py. Every number is a simulation of the firmware code on this twin, not a measurement on a real oven.
struct Pt { double t, temp, sp, duty; };
struct PredInfo { double activeS, firstS, rms; int state, fallbacks; };
static PredInfo gLast{};
static std::vector<Pt> parse(const std::string &s) {
  std::vector<Pt> v; std::stringstream in(s); std::string line;
  while (std::getline(in, line)) {
    std::vector<std::string> f; std::stringstream ls(line); std::string x;
    while (std::getline(ls, x, ',')) f.push_back(x);
    if (f.size() < 7) continue;
    v.push_back({atof(f[2].c_str()), atof(f[3].c_str()), atof(f[5].c_str()), atof(f[6].c_str())});
  }
  return v;
}
struct M { double over = -9, under = 9, recov = 0, inband = 0, duty = 0, tOver = 0, in10 = 0, mean = 0, tailMean = 0, tailIn10 = 0; };
static M measure(const std::vector<Pt> &v, double from, double band, double to = 1e18) {
  M m; int n = 0, inb = 0, in10 = 0; double lastOut = from, dsum = 0, esum = 0; double tmax = from; for (auto &p : v) if (p.t >= from && p.t <= to) tmax = std::max(tmax, p.t); int tn = 0, tin = 0; double tsum = 0;
  for (auto &p : v) {
    if (p.t < from || p.t > to) continue;
    double d = p.temp - p.sp; if (d > m.over) { m.over = d; m.tOver = p.t - from; } m.under = std::min(m.under, d);
    ++n; if (std::fabs(d) <= band) ++inb; else lastOut = p.t; dsum += p.duty; esum += d; if (std::fabs(d) <= 0.1) ++in10;
    if (p.t >= tmax - 3600.0) { ++tn; tsum += d; if (std::fabs(d) <= 0.1) ++tin; }
  }
  m.in10 = n ? 100.0 * in10 / n : 0; m.mean = n ? esum / n : 0; m.tailMean = tn ? tsum / tn : 0; m.tailIn10 = tn ? 100.0 * tin / tn : 0;
  m.recov = lastOut - from; m.inband = n ? 100.0 * inb / n : 0; m.duty = n ? 100.0 * dsum / n : 0; return m;
}
struct O { double Cf, Ce, hA, fanW, eggEnd, dead, heaterW; };
static Plant mk(const O &o, double amb, double eggStart, double ceScale = 1.0) {
  Plant p; p.eff = 1.0; p.heaterW = o.heaterW; p.capacity = o.Cf; p.eggCap = o.Ce * ceScale; p.hA = o.hA; p.fanW = o.fanW; p.eggW = 0; p.eggStart = eggStart;
  p.dead = o.dead; p.lag = 40; p.ambient = amb; p.resolution = 0.1; p.ventG = 200;
  // loss chosen so that the LATE-stage hold (embryo heat on) is 3 % of the bank: UA*dT = 0.03*P + fan + embryo
  p.loss = (0.03 * o.heaterW + o.fanW + o.eggEnd) / (37.5 - 25.0);
  return p;
}
static Scenario base(const Plant &p, double start) {
  Scenario sc; sc.sp = 37.5f; sc.mode = getenv("MODE") && std::string(getenv("MODE")) == "predictive" ? Mode::Predictive : (getenv("MODE") && std::string(getenv("MODE")) == "smart" ? Mode::Smart : Mode::Adaptive); sc.startTemp = start; sc.gainsSet = true;
  const double G = p.khTrue();
  sc.kp = 60.0 / (G * (p.dead + p.lag)); sc.ki = sc.kp / 250.0f; sc.kd = 0.0f; return sc;
}
static const char *tag = "";
int main(int argc, char **argv) {
  int part = atoi(argv[1]), parts = atoi(argv[2]); double HW = argc > 3 ? atof(argv[3]) : 20000.0;
  std::vector<O> os;
  for (double Cf : {0.3e6, 0.5e6, 0.8e6}) for (double hA : {1000.0, 1700.0}) for (double fan : {1500.0, 2000.0}) for (double ee : {0.0, 1500.0}) for (double D : {20.0, 60.0, 120.0})
    os.push_back({Cf, 1.9e6, hA, fan, ee, D, HW});
  printf("scn,Cf,hA,fan,eggEnd,dead,over,under,recov_s,inband_pct,high,reach_s,duty_pct,tOver_s,predActive_s,predFirst_s,predRms,predState,predFallbacks,in10_pct,mean_err,tail_mean_err,tail_in10_pct\n");
  for (size_t i = part; i < os.size(); i += parts) {
    if (getenv("STRIDE") && (i % atoi(getenv("STRIDE"))) != (getenv("PHASE") ? atoi(getenv("PHASE")) : 0)) continue;
    const O &o = os[i];
    const std::string SCN = getenv("SCN") ? getenv("SCN") : "123456";
    auto want = [&](char c) { return SCN.find(c) != std::string::npos; };
    auto row = [&](const char *n, const M &m, int high, double reach) {
      printf("%s,%.0f,%.0f,%.0f,%.0f,%.0f,%.3f,%.3f,%.0f,%.1f,%d,%.0f,%.2f,%.0f,%.0f,%.0f,%.3f,%d,%d,%.1f,%.3f,%.3f,%.1f\n", n, o.Cf, o.hA, o.fanW, o.eggEnd, o.dead, m.over, m.under, m.recov, m.inband, high, reach, m.duty, m.tOver, gLast.activeS, gLast.firstS, gLast.rms, gLast.state, gLast.fallbacks, m.in10, m.mean, m.tailMean, m.tailIn10);
      fflush(stdout);
    };
    // ---- T1 first batch: air and eggs at 28 C, no plan, 4 h ----
    const double t1Start = getenv("T1START") ? atof(getenv("T1START")) : 28.0;      // air and eggs at the start of the first batch
    Plant p = mk(o, 25, t1Start);
    Scenario s1 = base(p, t1Start); s1.durationS = 14400;
    std::stringstream t1; s1.trace = true; s1.traceOut = &t1; s1.label = "T";
    Result r1 = run(p, s1); gLast = {r1.predActiveS, r1.predFirstActiveS, r1.predRms, r1.predState, r1.predFallbacks}; auto v1 = parse(t1.str());
    double reach1 = -1; for (auto &q : v1) if (q.temp >= 37.2) { reach1 = q.t; break; }
    row("T1_batch1", measure(v1, reach1 > 0 ? reach1 : 14400, 0.3), r1.high, reach1);
    if (!r1.planOffered) { printf("noplan\n"); continue; }
    auto withPlan = [&](Scenario s) { s.planSet = true; s.plan = r1.planOut; return s; };
    // ---- T2 batch 2 with plan ----
    if (want('2')) {
      Scenario s = withPlan(base(p, t1Start)); s.durationS = 14400;
      std::stringstream t; s.trace = true; s.traceOut = &t; s.label = "T";
      Result r = run(p, s); gLast = {r.predActiveS, r.predFirstActiveS, r.predRms, r.predState, r.predFallbacks}; auto v = parse(t.str());
      double rc = -1; for (auto &q : v) if (q.temp >= 37.2) { rc = q.t; break; }
      row("T2_batch2", measure(v, rc > 0 ? rc : 14400, 0.3), r.high, rc);
    }
    // ---- T3 kick ----
    if (want('3')) {
      Scenario s = withPlan(base(p, 28.0)); s.spBefore = 37.4f; s.eventAtS = 10800; s.durationS = 10800 + 7200; s.event = "setpoint_step";
      std::stringstream t; s.trace = true; s.traceOut = &t; s.label = "T";
      Result r = run(p, s); gLast = {r.predActiveS, r.predFirstActiveS, r.predRms, r.predState, r.predFallbacks}; auto v = parse(t.str());
      row("T3_kick", measure(v, 10800, 0.1), r.high, 0);
    }
    // ---- T4 door ----
    if (want('4')) {
      Scenario s = withPlan(base(p, 28.0)); s.event = "door"; s.eventAtS = 10800; s.eventValue = 3.0; s.durationS = 10800 + 7200;
      std::stringstream t; s.trace = true; s.traceOut = &t; s.label = "T";
      Result r = run(p, s); gLast = {r.predActiveS, r.predFirstActiveS, r.predRms, r.predState, r.predFallbacks}; auto v = parse(t.str());
      std::vector<Pt> after; for (auto &q : v) if (q.t > 11100) after.push_back(q);
      M m = measure(v, 10800, 0.3); M ma = measure(after, 11100, 0.3); m.over = ma.over; m.tOver = ma.tOver;
      row("T4_door3", m, r.high, 0);
    }
    // ---- T5 ----
    if (want('5')) {
      Plant pr = p; pr.eggW = 0; pr.eggWEnd = o.eggEnd; pr.eggRampS = 8 * 3600;
      Scenario s = withPlan(base(pr, 28.0)); s.durationS = 3 * 3600 + 8 * 3600; s.ventProfile = true; s.ventCycleMin = 40; s.ventDutyPct = 18;
      std::stringstream t; s.trace = true; s.traceOut = &t; s.label = "T";
      Result r = run(pr, s); gLast = {r.predActiveS, r.predFirstActiveS, r.predRms, r.predState, r.predFallbacks}; auto v = parse(t.str());
      row("T5_embryoRamp", measure(v, 10800, 0.3), r.high, 0);
    }
    // ---- T6 ----
    if (want('6')) {
      Plant pr = p; pr.eggW = o.eggEnd;
      Scenario s = withPlan(base(pr, 28.0)); s.changeAtS = 10800; s.ambientDelta = 5; s.durationS = 10800 + 7200;
      std::stringstream t; s.trace = true; s.traceOut = &t; s.label = "T";
      Result r = run(pr, s); gLast = {r.predActiveS, r.predFirstActiveS, r.predRms, r.predState, r.predFallbacks}; auto v = parse(t.str());
      row("T6_amb+5", measure(v, 10800, 0.3), r.high, 0);
    }
    if (want('7')) {
      Plant pr = p; pr.eggW = o.eggEnd;
      Scenario s = withPlan(base(pr, 28.0)); s.changeAtS = 10800; s.ambientDelta = 5; s.ambientRampS = 7200; s.durationS = 10800 + 14400;
      std::stringstream t; s.trace = true; s.traceOut = &t; s.label = "T";
      Result r = run(pr, s); gLast = {r.predActiveS, r.predFirstActiveS, r.predRms, r.predState, r.predFallbacks}; auto v = parse(t.str());
      row("T7_ambRamp", measure(v, 10800, 0.3), r.high, 0);
    }
    (void)tag;
  }
}
