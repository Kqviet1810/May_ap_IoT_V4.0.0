#include "thermal-adaptive-sim.h"
#include <cstdio>
#include <sstream>
#include <vector>
#include <algorithm>
#include <cmath>
using namespace sim;
// Digital twin of the user's oven (2.5 m x 2 m chamber, 4 x 5 kW, 4 trolleys / ~10000 eggs, 2 kW circulation motor).
// usage: twin part parts [heaterW]
struct Pt { double t, temp, sp, duty; };
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
struct M { double over, under, recov, inband, duty, tOver; };
static M measure(const std::vector<Pt> &v, double from, double band, double to = 1e18) {
  M m{-9, 9, 0, 0, 0, 0}; int n = 0, inb = 0; double lastOut = from, dsum = 0;
  for (auto &p : v) {
    if (p.t < from || p.t > to) continue;
    double d = p.temp - p.sp; if (d > m.over) { m.over = d; m.tOver = p.t - from; } m.under = std::min(m.under, d);
    ++n; if (std::fabs(d) <= band) ++inb; else lastOut = p.t; dsum += p.duty;
  }
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
  Scenario sc; sc.sp = 37.5f; sc.mode = Mode::Adaptive; sc.startTemp = start; sc.gainsSet = true;
  const double G = p.khTrue();
  sc.kp = 60.0 / (G * (p.dead + p.lag)); sc.ki = sc.kp / 250.0f; sc.kd = 0.0f; return sc;
}
static const char *tag = "";
int main(int argc, char **argv) {
  int part = atoi(argv[1]), parts = atoi(argv[2]); double HW = argc > 3 ? atof(argv[3]) : 20000.0;
  std::vector<O> os;
  for (double Cf : {0.3e6, 0.5e6, 0.8e6}) for (double hA : {1000.0, 1700.0}) for (double fan : {1500.0, 2000.0}) for (double ee : {0.0, 1500.0}) for (double D : {20.0, 60.0, 120.0})
    os.push_back({Cf, 1.9e6, hA, fan, ee, D, HW});
  printf("scn,Cf,hA,fan,eggEnd,dead,over,under,recov_s,inband_pct,high,reach_s,duty_pct,tOver_s\n");
  for (size_t i = part; i < os.size(); i += parts) {
    const O &o = os[i];
    auto row = [&](const char *n, const M &m, int high, double reach) {
      printf("%s,%.0f,%.0f,%.0f,%.0f,%.0f,%.3f,%.3f,%.0f,%.1f,%d,%.0f,%.2f,%.0f\n", n, o.Cf, o.hA, o.fanW, o.eggEnd, o.dead, m.over, m.under, m.recov, m.inband, high, reach, m.duty, m.tOver);
      fflush(stdout);
    };
    // ---- T1 first batch: air and eggs at 28 C, no plan, 4 h ----
    Plant p = mk(o, 25, 28.0);
    Scenario s1 = base(p, 28.0); s1.durationS = 14400;
    std::stringstream t1; s1.trace = true; s1.traceOut = &t1; s1.label = "T";
    Result r1 = run(p, s1); auto v1 = parse(t1.str());
    double reach1 = -1; for (auto &q : v1) if (q.temp >= 37.2) { reach1 = q.t; break; }
    row("T1_batch1", measure(v1, reach1 > 0 ? reach1 : 14400, 0.3), r1.high, reach1);
    if (!r1.planOffered) { printf("noplan\n"); continue; }
    auto withPlan = [&](Scenario s) { s.planSet = true; s.plan = r1.planOut; return s; };
    // ---- T2 batch 2 with plan ----
    {
      Scenario s = withPlan(base(p, 28.0)); s.durationS = 14400;
      std::stringstream t; s.trace = true; s.traceOut = &t; s.label = "T";
      Result r = run(p, s); auto v = parse(t.str());
      double rc = -1; for (auto &q : v) if (q.temp >= 37.2) { rc = q.t; break; }
      row("T2_batch2", measure(v, rc > 0 ? rc : 14400, 0.3), r.high, rc);
    }
    // ---- T3 kick 37.4 -> 37.5 once the hold is reached (event at 3 h) ----
    {
      Scenario s = withPlan(base(p, 28.0)); s.spBefore = 37.4f; s.eventAtS = 10800; s.durationS = 10800 + 7200; s.event = "setpoint_step";
      std::stringstream t; s.trace = true; s.traceOut = &t; s.label = "T";
      Result r = run(p, s); auto v = parse(t.str());
      row("T3_kick", measure(v, 10800, 0.1), r.high, 0);
    }
    // ---- T4 door -3 C at 3 h ----
    {
      Scenario s = withPlan(base(p, 28.0)); s.event = "door"; s.eventAtS = 10800; s.eventValue = 3.0; s.durationS = 10800 + 7200;
      std::stringstream t; s.trace = true; s.traceOut = &t; s.label = "T";
      Result r = run(p, s); auto v = parse(t.str());
      std::vector<Pt> after; for (auto &q : v) if (q.t > 11100) after.push_back(q);
      M m = measure(v, 10800, 0.3); M ma = measure(after, 11100, 0.3); m.over = ma.over; m.tOver = ma.tOver;
      row("T4_door3", m, r.high, 0);
    }
    // ---- T5 late-stage hold: embryo heat ramps 0 -> eggEnd over 8 h while holding 37.5 (days ~9-18) ----
    {
      Plant pr = p; pr.eggW = 0; pr.eggWEnd = o.eggEnd; pr.eggRampS = 8 * 3600;
      Scenario s = withPlan(base(pr, 28.0)); s.durationS = 3 * 3600 + 8 * 3600; s.ventProfile = true; s.ventCycleMin = 40; s.ventDutyPct = 18;
      std::stringstream t; s.trace = true; s.traceOut = &t; s.label = "T";
      Result r = run(pr, s); auto v = parse(t.str());
      row("T5_embryoRamp", measure(v, 10800, 0.3), r.high, 0);
    }
    // ---- T6 ambient +5 C (room warms) at 3 h, late-stage ----
    {
      Plant pr = p; pr.eggW = o.eggEnd;
      Scenario s = withPlan(base(pr, 28.0)); s.changeAtS = 10800; s.ambientDelta = 5; s.durationS = 10800 + 7200;
      std::stringstream t; s.trace = true; s.traceOut = &t; s.label = "T";
      Result r = run(pr, s); auto v = parse(t.str());
      row("T6_amb+5", measure(v, 10800, 0.3), r.high, 0);
    }
    (void)tag;
  }
}
