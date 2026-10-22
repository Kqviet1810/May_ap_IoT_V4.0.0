#include "thermal-adaptive-sim.h"
#include <cstdio>
#include <sstream>
#include <vector>
#include <algorithm>
#include <cmath>
using namespace sim;
// usage: base part parts   -> baseline suite, one CSV row per (oven, scenario)
struct Pt { double t, temp, sp, duty; };
static std::vector<Pt> parse(const std::string &s) {
  std::vector<Pt> v; std::stringstream in(s); std::string line;
  while (std::getline(in, line)) {
    // label,mode,t,temp,fed,sp,duty,...
    std::vector<std::string> f; std::stringstream ls(line); std::string x;
    while (std::getline(ls, x, ',')) f.push_back(x);
    if (f.size() < 7) continue;
    v.push_back({atof(f[2].c_str()), atof(f[3].c_str()), atof(f[5].c_str()), atof(f[6].c_str())});
  }
  return v;
}
struct M { double over, under, recov, inband, highs, duty; };
// metrics over [from, end]: deviation from SP
static M measure(const std::vector<Pt> &v, double from, double band) {
  M m{-9, 9, 0, 0, 0, 0}; int n = 0, inb = 0; double lastOut = from; double dutySum = 0;
  for (auto &p : v) {
    if (p.t < from) continue;
    double d = p.temp - p.sp; m.over = std::max(m.over, d); m.under = std::min(m.under, d);
    ++n; if (std::fabs(d) <= band) ++inb; else lastOut = p.t; dutySum += p.duty;
  }
  m.recov = lastOut - from; m.inband = n ? 100.0 * inb / n : 0; m.duty = n ? dutySum / n : 0; return m;
}
static Plant mk(double G, double D, double lag, double hold, double res, double amb, double capScale) {
  Plant p; p.eff = 1.0; p.capacity = 16000.0 / G * capScale; p.dead = D; p.lag = lag; p.ambient = amb; p.resolution = res;
  p.loss = hold * 16000.0 / (37.5 - 25.0); return p;   // hold fixed at 25 C reference, as in the earlier grids
}
static Scenario base(double G, double D, double start) {
  Scenario sc; sc.sp = 37.5f; sc.mode = Mode::Adaptive; sc.startTemp = start; sc.gainsSet = true;
  sc.kp = 60.0 / (G * (D + 40.0)); sc.ki = sc.kp / 250.0f; sc.kd = 0.0f; return sc;
}
int main(int argc, char **argv) {
  int part = atoi(argv[1]), parts = atoi(argv[2]);
  struct O { double G, D, hold; }; std::vector<O> os;
  for (double G : {0.008, 0.012, 0.016, 0.025}) for (double D : {100.0, 140.0, 180.0}) for (double h : {0.12, 0.20, 0.28}) os.push_back({G, D, h});
  printf("scn,G,D,hold,over,under,recov_s,inband_pct,high,reach03_s,duty\n");
  for (size_t i = part; i < os.size(); i += parts) {
    const O &o = os[i]; const double lag = 40.0, res = 0.1;
    // ---- S1 cold start, no plan (batch 1) ----
    Plant p = mk(o.G, o.D, lag, o.hold, res, 25, 1.0);
    Scenario s1 = base(o.G, o.D, 29.0); s1.durationS = 7200; s1.metricsFromS = 3600;
    std::stringstream t1; s1.trace = true; s1.traceOut = &t1; s1.label = "T";
    Result r1 = run(p, s1); auto v1 = parse(t1.str());
    double reach1 = -1; for (auto &q : v1) if (q.temp >= 37.2) { reach1 = q.t; break; }
    // overshoot / band measured from the moment SP-0.3 is reached
    M m1 = measure(v1, reach1 > 0 ? reach1 : 7200, 0.3);
    printf("S1_cold,%.3f,%.0f,%.2f,%.3f,%.3f,%.0f,%.1f,%d,%.0f,%.3f\n", o.G, o.D, o.hold, m1.over, m1.under, m1.recov, m1.inband, r1.high, reach1, m1.duty);
    fflush(stdout);
    if (!r1.planOffered) { printf("noplan,%.3f,%.0f,%.2f,0,0,0,0,0,0,0\n", o.G, o.D, o.hold); continue; }
    auto withPlan = [&](Scenario s) { s.planSet = true; s.plan = r1.planOut; return s; };
    // ---- S2 batch 2 with plan, cold ----
    {
      Scenario s = withPlan(base(o.G, o.D, 29.0)); s.durationS = 7200; s.metricsFromS = 3600;
      std::stringstream t; s.trace = true; s.traceOut = &t; s.label = "T";
      Result r = run(p, s); auto v = parse(t.str());
      double rc = -1; for (auto &q : v) if (q.temp >= 37.2) { rc = q.t; break; }
      M m = measure(v, rc > 0 ? rc : 7200, 0.3);
      printf("S2_batch2,%.3f,%.0f,%.2f,%.3f,%.3f,%.0f,%.1f,%d,%.0f,%.3f\n", o.G, o.D, o.hold, m.over, m.under, m.recov, m.inband, r.high, rc, m.duty); fflush(stdout);
    }
    // ---- S3 kick 37.4 -> 37.5 in a running oven (plan known, chamber already at 37.4) ----
    {
      Scenario s = withPlan(base(o.G, o.D, 29.0)); s.spBefore = 37.4f; s.eventAtS = 5400; s.durationS = 5400 + 5400; s.event = "setpoint_step";
      std::stringstream t; s.trace = true; s.traceOut = &t; s.label = "T";
      Result r = run(p, s); auto v = parse(t.str());
      M m = measure(v, 5400, 0.1);
      printf("S3_kick,%.3f,%.0f,%.2f,%.3f,%.3f,%.0f,%.1f,%d,0,%.3f\n", o.G, o.D, o.hold, m.over, m.under, m.recov, m.inband, r.high, m.duty); fflush(stdout);
    }
    // ---- S4 door: -2 C and -5 C in the hold (cold start with plan, event at 90 min) ----
    for (double drop : {2.0, 5.0}) {
      Scenario s = withPlan(base(o.G, o.D, 29.0)); s.event = "door"; s.eventAtS = 5400; s.eventValue = drop; s.durationS = 5400 + 5400;
      std::stringstream t; s.trace = true; s.traceOut = &t; s.label = "T";
      Result r = run(p, s); auto v = parse(t.str());
      M m = measure(v, 5400, 0.3);
      // overshoot AFTER the door: skip the first 300 s (the drop itself)
      std::vector<Pt> after; for (auto &q : v) if (q.t > 5700) after.push_back(q);
      M ma = measure(after, 5700, 0.3);
      printf("S4_door%.0f,%.3f,%.0f,%.2f,%.3f,%.3f,%.0f,%.1f,%d,0,%.3f\n", drop, o.G, o.D, o.hold, ma.over, m.under, m.recov, m.inband, r.high, m.duty); fflush(stdout);
    }
    // ---- S5 ambient step +5 / -5 C in the hold ----
    for (double da : {5.0, -5.0}) {
      Scenario s = withPlan(base(o.G, o.D, 29.0)); s.changeAtS = 5400; s.ambientDelta = da; s.durationS = 5400 + 7200;
      std::stringstream t; s.trace = true; s.traceOut = &t; s.label = "T";
      Result r = run(p, s); auto v = parse(t.str());
      M m = measure(v, 5400, 0.3);
      printf("S5_amb%+.0f,%.3f,%.0f,%.2f,%.3f,%.3f,%.0f,%.1f,%d,0,%.3f\n", da, o.G, o.D, o.hold, m.over, m.under, m.recov, m.inband, r.high, m.duty); fflush(stdout);
    }
    // ---- S6 chamber load x0.5 / x1.5 at batch 2 ----
    for (double cs : {0.5, 1.5}) {
      Plant p2 = mk(o.G, o.D, lag, o.hold, res, 25, cs);
      Scenario s = withPlan(base(o.G, o.D, 29.0)); s.durationS = 7200;
      std::stringstream t; s.trace = true; s.traceOut = &t; s.label = "T";
      Result r = run(p2, s); auto v = parse(t.str());
      double rc = -1; for (auto &q : v) if (q.temp >= 37.2) { rc = q.t; break; }
      M m = measure(v, rc > 0 ? rc : 7200, 0.3);
      printf("S6_load%.1f,%.3f,%.0f,%.2f,%.3f,%.3f,%.0f,%.1f,%d,%.0f,%.3f\n", cs, o.G, o.D, o.hold, m.over, m.under, m.recov, m.inband, r.high, rc, m.duty); fflush(stdout);
    }
    // ---- S7 long hold: 6 h after the first 1.5 h (plan known) ----
    {
      Scenario s = withPlan(base(o.G, o.D, 29.0)); s.durationS = 6 * 3600 + 5400;
      std::stringstream t; s.trace = true; s.traceOut = &t; s.label = "T";
      Result r = run(p, s); auto v = parse(t.str());
      M m = measure(v, 5400, 0.3);
      M m2 = measure(v, 5400, 0.15);
      printf("S7_long,%.3f,%.0f,%.2f,%.3f,%.3f,%.0f,%.1f,%d,0,%.3f\n", o.G, o.D, o.hold, m.over, m.under, m.recov, m2.inband, r.high, m.duty); fflush(stdout);
    }
  }
}
