// Established-method baseline: FOPDT identification + SIMC PI + Smith predictor, on the same standalone two-mass twin as pred_twin.cpp
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <cmath>
using namespace std;
struct Rec { double t, ta; };
struct M { double over, under, recov, inband, tailMean, tailIn10; };
static M measure(const vector<Rec> &v, double from, double sp, double band) {
  M m{-9, 9, 0, 0, 0, 0}; int n = 0, inb = 0; double lastOut = from, tmax = from;
  for (auto &p : v) tmax = max(tmax, p.t);
  int tn = 0, tin = 0; double tsum = 0;
  for (auto &p : v) { if (p.t < from) continue; double d = p.ta - sp; m.over = max(m.over, d); m.under = min(m.under, d); ++n; if (fabs(d) <= band) ++inb; else lastOut = p.t;
    if (p.t >= tmax - 3600) { ++tn; tsum += d; if (fabs(d) <= 0.1) ++tin; } }
  m.recov = lastOut - from; m.inband = n ? 100.0 * inb / n : 0; m.tailMean = tn ? tsum / tn : 0; m.tailIn10 = tn ? 100.0 * tin / tn : 0; return m;
}
struct Cfg { double Cf, hA, fan, eggEnd, dead; };
// ---- the controller ----
struct Fopdt { double K = 0.12, tau = 400, th = 60, Tb = 25; };
struct Smith {
  static constexpr double dt = 2.0; static constexpr int N = 900;
  vector<double> u, y; Fopdt p; bool ident = false; double I = 0, ym0 = 0, ymd = 0; vector<double> ud; double lastU = 0; int fit = 0; double rms = 9;
  double sse(const Fopdt &q, int n) const {
    double ym = y[max(0, n - N)], s = 0; int cnt = 0; int s0 = max(0, n - N);
    for (int i = s0; i < n; ++i) { int j = i - (int)(q.th / dt); double uu = j >= 0 ? u[j] : 0; ym += dt * (-(ym - q.Tb) + q.K * uu) / q.tau; double e = ym - y[i]; if (i > s0) { s += e * e; ++cnt; } }
    return cnt ? sqrt(s / cnt) : 9;
  }
  void refit() {
    int n = y.size(); if (n < 150) return; Fopdt best = p; double bc = sse(best, n);
    double st[4] = {0.3, 0.3, 0.3, 1.0};
    for (int it = 0; it < 80; ++it) { int c = it % 4; for (int dir = -1; dir <= 1; dir += 2) { Fopdt q = best;
        if (c == 0) q.K = min(0.6, max(0.01, q.K * (1 + dir * st[0]))); if (c == 1) q.tau = min(8000.0, max(60.0, q.tau * (1 + dir * st[1]))); if (c == 2) q.th = min(300.0, max(6.0, q.th * (1 + dir * st[2]))); if (c == 3) q.Tb = min(36.0, max(15.0, q.Tb + dir * st[3]));
        double cc = sse(q, n); if (cc < bc) { bc = cc; best = q; } }
      if (it % 4 == 3) for (double &s : st) s *= 0.8; }
    p = best; rms = bc; if (n >= 300 && bc < 0.15) ident = true; if (bc > 0.4) ident = false;
  }
  // returns duty %, limit 100
  double step(double sp, double pv, double limit) {
    y.push_back(pv); u.push_back(lastU); if ((int)y.size() % 15 == 0) refit();
    if (!ident) { lastU = min(40.0, max(0.0, 15.0 * (sp - pv))); ym0 = ymd = pv; I = lastU; ud.assign(1, 0); return lastU; }
    // Smith predictor: undelayed and delayed model
    int nd = max(1, (int)(p.th / dt)); ud.push_back(lastU);
    ym0 += dt * (-(ym0 - p.Tb) + p.K * lastU) / p.tau;
    double udel = (int)ud.size() > nd ? ud[ud.size() - 1 - nd] : 0; ymd += dt * (-(ymd - p.Tb) + p.K * udel) / p.tau;
    double ypred = pv + (ym0 - ymd);
    double tauc = max(p.th, 60.0), Kc = p.tau / (p.K * (tauc + p.th)), Ti = min(p.tau, 4 * (tauc + p.th));
    double e = sp - ypred; double un = Kc * e + I;
    double uc = min(limit, max(0.0, un));
    if (un == uc || (un > uc && e < 0) || (un < uc && e > 0)) I += Kc / Ti * e * dt;      // conditional integration
    I = min(100.0, max(-100.0, I));
    if (pv > sp + 0.3) uc = 0; lastU = uc; return uc;
  }
};
static double gHW = 20000;
static pair<M, int> run(const Cfg &c, int kind) {
  const double Pw = gHW, Ce = 1.9e6, lag = 40, ventG = 200, amb0 = 25, UA = (0.03 * Pw + c.fan + c.eggEnd) / 12.5;
  double Ta = 28, Te = 28, hl = 0, ventAir = 0, amb = amb0; vector<double> pipe((int)(c.dead * 10) + 1, 0.0); size_t pp = 0;
  const double dur = kind == 1 ? 14400 : (kind == 5 ? 11 * 3600 : 18000);
  Smith ctl; double duty = 0, sdAcc = 0, sensed = 28, ventHold = 0; bool ventOn = false; vector<Rec> rec; int highs = 0; bool prevHigh = false;
  for (long it = 0; it < (long)(dur * 10); ++it) { const double t = it * 0.1;
    if (kind == 6 && t >= 10800) amb = amb0 + 5; if (kind == 4 && it == 108000) Ta -= 3.0;
    const double eggW = kind == 5 ? c.eggEnd * min(1.0, t / (8 * 3600.0)) : (kind == 6 ? c.eggEnd : 0.0);
    sdAcc += duty / 100.0; const bool ssr = sdAcc >= 1.0; if (ssr) sdAcc -= 1.0;
    if (it % 20 == 0) { sensed = round(Ta / 0.1) * 0.1; duty = ctl.step(37.5, sensed, 100.0);
      if (sensed >= 38.0) { ventOn = true; ventHold = t + 120; } else if (ventOn && t >= ventHold && sensed <= 37.6) ventOn = false; }
    pipe[pp] = ssr ? Pw : 0.0; pp = (pp + 1) % pipe.size(); hl += (pipe[pp] - hl) * 0.1 / lag; ventAir += ((ventOn ? 1.0 : 0.0) - ventAir) * 0.1 / 8.0;
    const double q = c.hA * (Ta - Te); Ta += (hl - UA * (Ta - amb) - ventG * ventAir * (Ta - amb) - q + c.fan) * 0.1 / c.Cf; Te += (q + eggW) * 0.1 / Ce;
    if (it % 100 == 0) rec.push_back({t, Ta}); const bool hi = Ta >= 38.2; if (hi && !prevHigh) ++highs; prevHigh = hi;
  }
  double reach = -1; for (auto &r : rec) if (r.ta >= 37.2) { reach = r.t; break; }
  double from = kind == 1 ? (reach > 0 ? reach : dur) : 10800;
  return {measure(rec, from, 37.5, 0.3), highs};
}
int main(int argc, char **argv) {
  int stride = argc > 1 ? atoi(argv[1]) : 5; gHW = argc > 2 ? atof(argv[2]) : 20000; int part = argc > 3 ? atoi(argv[3]) : 0, parts = argc > 4 ? atoi(argv[4]) : 1;
  vector<Cfg> cs; for (double Cf : {0.3e6, 0.5e6, 0.8e6}) for (double hA : {1000.0, 1700.0}) for (double fan : {1500.0, 2000.0}) for (double ee : {0.0, 1500.0}) for (double D : {20.0, 60.0, 120.0}) cs.push_back({Cf, hA, fan, ee, D});
  printf("scn,Cf,hA,fan,eggEnd,dead,over,under,recov_s,inband,high,tail_mean,tail_in10\n");
  const char *nm[] = {"", "T1", "", "", "T4", "T5", "T6"};
  for (size_t i = part; i < cs.size(); i += parts) { if (i % stride) continue;
    for (int kind : {1, 4, 5, 6}) { auto r = run(cs[i], kind); printf("%s,%.0f,%.0f,%.0f,%.0f,%.0f,%.3f,%.3f,%.0f,%.1f,%d,%.3f,%.1f\n", nm[kind], cs[i].Cf, cs[i].hA, cs[i].fan, cs[i].eggEnd, cs[i].dead, r.first.over, r.first.under, r.first.recov, r.first.inband, r.second, r.first.tailMean, r.first.tailIn10); fflush(stdout); } }
}
