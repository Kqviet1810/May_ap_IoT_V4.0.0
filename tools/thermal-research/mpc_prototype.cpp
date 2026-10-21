// Two-node predictive controller prototype ("energy-ledger MPC") on the digital twin of the user's oven.
// est=0: controller reads the TRUE states (oracle ceiling). est=1: it sees only the quantised air temperature and estimates the
// egg temperature and the unknown power into the eggs with a Kalman filter. mHA/mCe/mUA/mCf scale the controller's BELIEF
// of the plant (model error); the plant itself is unchanged.
// usage: mpc part parts heaterW vent est mHA mCe mUA mCf
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <algorithm>
struct Par { double Cf, Ce, hA, UA, fan, Pw, lag, ventG; };
static const double DT = 30.0;
struct S { double Ta, Te, hl; double pipe[16]; int head, nd; };
static inline void step(const Par &p, S &s, double u, bool vent, double w, double amb) {
  const double delayed = s.pipe[s.head]; s.pipe[s.head] = u * p.Pw; s.head = (s.head + 1) % s.nd;
  s.hl += (delayed - s.hl) * DT / p.lag;
  const double ventW = vent ? p.ventG * (s.Ta - amb) : 0.0;
  const double q = p.hA * (s.Ta - s.Te);
  s.Ta += (s.hl - p.UA * (s.Ta - amb) - ventW - q + p.fan) * DT / p.Cf;
  s.Te += (q + w) * DT / p.Ce;
}
struct Plan { double u1, T1, tau; double Tv; };
static inline double tailU(const Par &p, const S &s, double sp, double amb, double tau) {
  const double Pn = p.UA * (s.Ta - amb) + p.hA * (s.Ta - s.Te) - p.fan + p.Cf * (sp - s.Ta) / tau;
  return std::min(1.0, std::max(0.0, Pn / p.Pw));
}
static double evalPlan(const Par &p, const S &s0, const Plan &pl, double sp, double w, double amb, bool allowVent) {
  S s = s0; double cost = 0;
  for (int k = 0; k < 240; ++k) {
    const double t = k * DT; const double u = t < pl.T1 * 60.0 ? pl.u1 : tailU(p, s, sp, amb, pl.tau);
    const bool vent = allowVent && t < pl.Tv * 60.0;
    step(p, s, u, vent, w, amb);
    const double e = s.Ta - sp;
    cost += (e > 0.03 ? 12.0 * e * e : e * e) + (e > 0.35 ? 400.0 * (e - 0.35) * (e - 0.35) : 0.0) + (vent ? 0.0005 : 0.0);
  }
  const double eT = s.Te - (sp + w / p.hA); cost += 8.0 * eT * eT;
  return cost;
}
struct KF {
  // states: Ta, Te, w (unknown power into the eggs), d (unknown power into the air: room temperature, door seals, ...)
  double x[4], P[4][4];
  void init(double ta) { x[0] = ta; x[1] = ta; x[2] = 0; x[3] = 0; for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) P[i][j] = 0; P[0][0] = 0.01; P[1][1] = 4.0; P[2][2] = 100.0 * 100.0; P[3][3] = 200.0 * 200.0; }
  void update(const Par &p, double hl, double amb, bool vent, double y, double dt) {
    const double vc = vent ? p.ventG : 0.0;
    const double a = dt / p.Cf, b = dt / p.Ce;
    double F[4][4] = {{1 - (p.UA + p.hA + vc) * a, p.hA * a, 0, a}, {p.hA * b, 1 - p.hA * b, b, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    const double u0 = (hl + p.fan + (p.UA + vc) * amb) * a;
    double xp[4]; for (int i = 0; i < 4; ++i) { xp[i] = 0; for (int j = 0; j < 4; ++j) xp[i] += F[i][j] * x[j]; } xp[0] += u0;
    const double inn = y - xp[0];
    if (std::fabs(inn) > 0.6) {           // a step the model cannot explain with a power change (door, sensor glitch): air-side event
      x[0] = y; x[1] = xp[1]; x[2] = xp[2]; x[3] = xp[3]; P[0][0] = 0.01; return;
    }
    double Pp[4][4], T[4][4];
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) { T[i][j] = 0; for (int k = 0; k < 4; ++k) T[i][j] += F[i][k] * P[k][j]; }
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) { Pp[i][j] = 0; for (int k = 0; k < 4; ++k) Pp[i][j] += T[i][k] * F[j][k]; }
    Pp[0][0] += 3e-5; Pp[1][1] += 3e-6; Pp[2][2] += 20.0 * (dt / 10.0); Pp[3][3] += 60.0 * (dt / 10.0);
    const double R = 0.03 * 0.03; const double S_ = Pp[0][0] + R;
    double K[4]; for (int i = 0; i < 4; ++i) K[i] = Pp[i][0] / S_;
    for (int i = 0; i < 4; ++i) x[i] = xp[i] + K[i] * inn;
    x[2] = std::min(4000.0, std::max(-500.0, x[2])); x[3] = std::min(4000.0, std::max(-4000.0, x[3]));
    double Pn[4][4]; for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) Pn[i][j] = Pp[i][j] - K[i] * Pp[0][j];
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) P[i][j] = Pn[i][j];
  }
};
struct M { double over, under, recov, inband, tOver; };
struct Rec { double t, ta; };
static M measure(const std::vector<Rec> &v, double from, double sp, double band) {
  M m{-9, 9, 0, 0, 0}; int n = 0, inb = 0; double lastOut = from;
  for (auto &p : v) { if (p.t < from) continue; double d = p.ta - sp; if (d > m.over) { m.over = d; m.tOver = p.t - from; } m.under = std::min(m.under, d); ++n; if (std::fabs(d) <= band) ++inb; else lastOut = p.t; }
  m.recov = lastOut - from; m.inband = n ? 100.0 * inb / n : 0; return m;
}
struct Cfg { double Cf, hA, fan, eggEnd, dead; };
static double gHW = 20000; static bool gVent = false, gEst = false; static double mHA = 1, mCe = 1, mUA = 1, mCf = 1;
static void run(const Cfg &c, int kind, const char *name) {
  Par t; t.Cf = c.Cf; t.Ce = 1.9e6; t.hA = c.hA; t.fan = c.fan; t.Pw = gHW; t.lag = 40; t.ventG = 200; t.UA = (0.03 * gHW + c.fan + c.eggEnd) / 12.5;
  Par b = t; b.hA *= mHA; b.Ce *= mCe; b.UA *= mUA; b.Cf *= mCf;   // belief
  const double dead = c.dead; const int nd = std::max(1, (int)std::lround(dead / DT));
  double dur = kind == 1 ? 14400 : (kind == 5 ? 3 * 3600 + 8 * 3600 : 10800 + 7200);
  double Ta = 28, Te = 28, hl = 0, ventAir = 0, amb = 25; std::vector<double> realPipe((size_t)dead + 1, 0.0); size_t rp = 0;
  double spCur = kind == 3 ? 37.4 : 37.5; double u = 0; bool vent = false; std::vector<Rec> rec; int highs = 0; bool prevHigh = false; double reach = -1;
  std::vector<double> cmdHist(nd, 0.0); int chead = 0; KF kf; kf.init(28); double bhl = 0; double sensed = 28; double dutySum = 0; long dutyN = 0;
  const int nd10 = std::max(3, 3 * nd); std::vector<double> bpipe(nd10, 0.0); int bhead = 0;
  for (double tt = 0; tt < dur; tt += 1.0) {
    if (kind == 3 && tt >= 10800) spCur = 37.5;
    if (kind == 6 && tt >= 10800) amb = 30;
    if (kind == 4 && std::fabs(tt - 10800) < 0.5) Ta -= 3.0;
    const double eggW = kind == 5 ? c.eggEnd * std::min(1.0, tt / (8 * 3600.0)) : (kind == 6 ? c.eggEnd : 0.0);
    if (std::fmod(tt, 2.0) < 0.5) sensed = std::round(Ta / 0.1) * 0.1;
    if (gEst && std::fmod(tt, 10.0) < 0.5) {
      const double delayed = bpipe[bhead]; bpipe[bhead] = u * t.Pw; bhead = (bhead + 1) % nd10;
      bhl += (delayed - bhl) * 10.0 / b.lag;
      kf.update(b, bhl, 25.0, vent, sensed, 10.0);
    }
    if (std::fmod(tt, DT) < 0.5) {
      S s0; s0.nd = nd; s0.head = 0;
      double ta, te, w, dA = 0;
      if (!gEst) { ta = Ta; te = Te; w = eggW; for (int k = 0; k < nd; ++k) s0.pipe[k] = cmdHist[(chead + k) % nd]; s0.hl = hl; }
      else {
        ta = kf.x[0]; te = kf.x[1]; w = kf.x[2]; dA = kf.x[3];
        // planner pipeline (30 s slots) from the estimator's 10 s pipeline: oldest first
        for (int k = 0; k < nd; ++k) { double acc = 0; for (int j = 0; j < 3; ++j) { const int idx = (bhead + k * 3 + j) % nd10; acc += bpipe[idx]; } s0.pipe[k] = acc / 3.0; }
        s0.hl = bhl;
      }
      s0.Ta = gEst ? sensed : ta; s0.Te = te; (void)ta;
      const double ambB = gEst ? 25.0 : amb;
      Par bd = b; bd.fan += dA;
      Plan best{0, 0, 300, 0}; double bc = 1e18;
      static const double U1[] = {0, 0.1, 0.2, 0.3, 0.45, 0.6, 0.8, 1.0};
      static const double T1[] = {0, 1, 2, 4, 8, 16};
      static const double TAU[] = {90, 240, 600, 1500};
      static const double TV[] = {0, 10, 30};
      for (double u1 : U1) for (double t1 : T1) for (double tau : TAU) for (double tv : TV) {
        if (!gVent && tv > 0) continue;
        if (t1 == 0 && u1 != 0) continue;
        Plan pl{u1, t1, tau, tv};
        const double c1 = evalPlan(bd, s0, pl, spCur, w, ambB, gVent);
        if (c1 < bc) { bc = c1; best = pl; }
      }
      double uNow = best.u1;
      if (best.T1 <= 0.0) uNow = tailU(bd, s0, spCur, ambB, best.tau);
      u = uNow; vent = gVent && best.Tv > 0 && sensed >= spCur - 0.05;   // guard: never cool when the air is already below SP
      cmdHist[chead] = u * t.Pw; chead = (chead + 1) % nd;
    }
    realPipe[rp] = u * t.Pw; rp = (rp + 1) % realPipe.size(); const double delayed = realPipe[rp];
    hl += (delayed - hl) * 1.0 / t.lag;
    ventAir += ((vent ? 1.0 : 0.0) - ventAir) / 8.0;
    const double ventW = t.ventG * ventAir * (Ta - amb);
    const double q = t.hA * (Ta - Te);
    Ta += (hl - t.UA * (Ta - amb) - ventW - q + t.fan) / t.Cf;
    Te += (q + eggW) / t.Ce;
    if (std::fmod(tt, 10.0) < 0.5) rec.push_back({tt, Ta});
    const bool hi = Ta >= 38.2; if (hi && !prevHigh) ++highs; prevHigh = hi;
    if (reach < 0 && Ta >= 37.2) reach = tt;
    dutySum += u; ++dutyN;
    if (getenv("TRACE") && kind == atoi(getenv("TRACE")) && std::fmod(tt, getenv("TS") ? atof(getenv("TS")) : 300.0) < 0.5 && tt >= (getenv("T0") ? atof(getenv("T0")) : 0.0)) printf("# t=%5.0f min=%3.0f Ta=%.2f Te=%.2f u=%.2f | KF Ta=%.2f Te=%.2f w=%.0f d=%.0f vent=%d\n", tt, tt / 60, Ta, Te, u, kf.x[0], kf.x[1], kf.x[2], kf.x[3], (int)vent);
  }
  M m;
  if (kind == 1) m = measure(rec, reach > 0 ? reach : dur, 37.5, 0.3);
  else if (kind == 3) m = measure(rec, 10800, 37.5, 0.1);
  else if (kind == 4) { std::vector<Rec> a; for (auto &r : rec) if (r.t > 11100) a.push_back(r); m = measure(rec, 10800, 37.5, 0.3); M ma = measure(a, 11100, 37.5, 0.3); m.over = ma.over; m.tOver = ma.tOver; }
  else m = measure(rec, 10800, 37.5, 0.3);
  printf("%s,%.0f,%.0f,%.0f,%.0f,%.0f,%.3f,%.3f,%.0f,%.1f,%d,%.0f,%.2f,%.0f\n", name, c.Cf, c.hA, c.fan, c.eggEnd, c.dead, m.over, m.under, m.recov, m.inband, highs, reach, 100.0 * dutySum / std::max(1L, dutyN), m.tOver);
  fflush(stdout);
}
int main(int argc, char **argv) {
  int part = atoi(argv[1]), parts = atoi(argv[2]); gHW = atof(argv[3]); gVent = atoi(argv[4]) != 0; gEst = atoi(argv[5]) != 0;
  if (argc > 9) { mHA = atof(argv[6]); mCe = atof(argv[7]); mUA = atof(argv[8]); mCf = atof(argv[9]); }
  std::vector<Cfg> cs;
  for (double Cf : {0.3e6, 0.5e6, 0.8e6}) for (double hA : {1000.0, 1700.0}) for (double fan : {1500.0, 2000.0}) for (double ee : {0.0, 1500.0}) for (double D : {20.0, 60.0, 120.0}) cs.push_back({Cf, hA, fan, ee, D});
  printf("scn,Cf,hA,fan,eggEnd,dead,over,under,recov_s,inband_pct,high,reach_s,duty_pct,tOver_s\n");
  if (getenv("ONE")) { int id = atoi(getenv("ONE")); run(cs[id], atoi(getenv("TRACE")), "one"); return 0; }
  for (size_t i = part; i < cs.size(); i += parts) {
    run(cs[i], 1, "T1_batch1"); run(cs[i], 3, "T3_kick"); run(cs[i], 4, "T4_door3"); run(cs[i], 5, "T5_embryoRamp"); run(cs[i], 6, "T6_amb+5");
  }
}
