// Fits ONE oven (the physical oven of the owner's machine) to several real logs at once, so that the fleet bench can be calibrated against what the machine
// really did. The model is the plant of tests/thermal-sim.h (two nodes: air and mass, a heater delay and lag, walls, the exhaust fan); the logged heater
// request is the input and the logged reading is what has to be reproduced, through the production sensor path (0.1 degC, median of 3, 3/8 IIR).
// Shared by all logs: heat capacity of the air + walls Cf, wall loss UA, the mass Ce and its coupling hA, the heater delay td and lag tl, the fan loss.
// One per log: the room temperature Tamb and the start offset of the mass.   Pmax is fixed (16 kW): only the ratios Pmax/Cf and UA/Cf can be known.
// Build: g++ -O2 -std=c++17 tools/thermal-research/fit-oven.cpp -o /tmp/fit-oven
// Run:   /tmp/fit-oven tests/fixtures/real-logs/mayap-20261031-afternoon.csv ...  (prints the fit; FIT_RESTARTS=n, FIT_ITERS=n)
#ifndef HDR
#define HDR "../../MAYAP_INDUSTRIAL_v1_0_0/thermal_v3.h"
#endif
#include HDR
#include "../../tests/thermal-sim.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>
using namespace thermalsim;

struct Log { std::string name; std::vector<double> t, pv, req, fan; double dtRow = 10.0; };
static bool loadLog(const std::string &path, Log &L) {
  std::ifstream f(path);
  if (!f) return false;
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#' || line.rfind("t_s", 0) == 0) continue;
    std::stringstream ss(line); std::string c; std::vector<double> v;
    while (std::getline(ss, c, ',')) v.push_back(atof(c.c_str()));
    if (v.size() < 4) continue;
    L.t.push_back(v[0]); L.pv.push_back(v[1]); L.req.push_back(v[2]); L.fan.push_back(v[3]);
  }
  L.name = path.substr(path.find_last_of('/') + 1);
  return L.t.size() > 20;
}

// The plant of thermal-sim.h at a coarser step (0.5 s) for the search; verified afterwards with the real class at 0.1 s.
struct Fit {
  double Pmax = 16000;
  struct Shared { double Cf, UA, Ce, hA, td, tl, ventW, tailFrac, tailTau, sensorLag, Q0; } s;
  struct PerLog { double Tamb, d; };
};
static const double DTF = 0.5;
static double simulate(const Fit::Shared &s, const Fit::PerLog &pl, const Log &L, std::vector<double> *out, double huber, double *nOut) {
  const size_t pn = std::max<size_t>(1, (size_t)std::lround(s.td / DTF));
  std::vector<double> pipe(pn, 0.0);
  size_t idx = 0;
  double Ta = L.pv[0], Te = L.pv[0] + pl.d, heater = 0, heater2 = 0, vent = 0, sens = L.pv[0];
  SensorChain ch(0.1); for (int i = 0; i < 8; ++i) ch.sample(Ta);
  size_t row = 0; double sensT = 0, cost = 0; long n = 0;
  const double tEnd = L.t.back();
  double sensorSampleAt = 0;
  for (double t = 0; t <= tEnd; t += DTF) {
    while (row + 1 < L.t.size() && L.t[row + 1] <= t) ++row;
    const double u = std::max(0.0, std::min(100.0, L.req[row]));
    const bool fan = L.fan[row] > 0.5;
    const double pwr = u / 100.0 * 16000.0;
    const double delayed = pipe[idx]; pipe[idx] = pwr; idx = (idx + 1) % pn;
    heater += (delayed - heater) * DTF / std::max(0.5, s.tl);
    heater2 += (delayed - heater2) * DTF / std::max(5.0, s.tailTau);
    const double eff = (1.0 - s.tailFrac) * heater + s.tailFrac * heater2;
    vent += ((fan ? 1.0 : 0.0) - vent) * DTF / 8.0;
    const double ventLoss = (s.ventW / 12.5) * vent * (Ta - pl.Tamb);
    const double qEgg = s.hA * (Ta - Te);
    Ta += (eff + s.Q0 - s.UA * (Ta - pl.Tamb) - qEgg - ventLoss) / s.Cf * DTF;
    Te += qEgg / s.Ce * DTF;
    sensT += DTF;
    sens += s.sensorLag > 0.5 ? (Ta - sens) * DTF / s.sensorLag : (Ta - sens);
    if (sensT >= sensorSampleAt) { ch.sample(sens); sensorSampleAt += 2.0; }
    // compare at the logged instants
    static thread_local size_t cmp = 0;
    (void)cmp;
    if (std::fmod(t, 10.0) < DTF * 0.5 && t > 0) {
      size_t r = (size_t)std::lround(t / 10.0);
      // find the logged row nearest to t
      size_t lo = 0, hi = L.t.size();
      while (lo + 1 < hi) { size_t m = (lo + hi) / 2; if (L.t[m] <= t) lo = m; else hi = m; }
      if (std::fabs(L.t[lo] - t) < 5.0) {
        const double e = ch.value() - L.pv[lo];
        const double a = std::fabs(e);
        cost += a <= huber ? 0.5 * e * e : huber * (a - 0.5 * huber);
        ++n;
        if (out) out->push_back(e);
      }
      (void)r;
    }
  }
  if (nOut) *nOut = (double)n;
  return n ? cost / (double)n : 1e9;
}

struct Vec { std::vector<double> x; };   // normalised [0,1]
struct Bounds { double lo, hi; bool logScale; };
static std::vector<Bounds> B;
static double toVal(size_t i, double u) { u = std::max(0.0, std::min(1.0, u)); return B[i].logScale ? std::exp(std::log(B[i].lo) + u * (std::log(B[i].hi) - std::log(B[i].lo))) : B[i].lo + u * (B[i].hi - B[i].lo); }
static void decode(const std::vector<double> &x, size_t nLogs, Fit::Shared &s, std::vector<Fit::PerLog> &pl) {
  s.Cf = toVal(0, x[0]); s.UA = toVal(1, x[1]); s.Ce = toVal(2, x[2]); s.hA = toVal(3, x[3]); s.td = toVal(4, x[4]); s.tl = toVal(5, x[5]); s.ventW = toVal(6, x[6]);
  s.tailFrac = toVal(7, x[7]); s.tailTau = toVal(8, x[8]); s.sensorLag = toVal(9, x[9]); s.Q0 = toVal(10, x[10]);
  const double tref = toVal(11, x[11]);
  pl.resize(nLogs);
  for (size_t k = 0; k < nLogs; ++k) { pl[k].Tamb = std::min(37.0, tref + toVal(12, x[12 + 2 * k])); pl[k].d = toVal(13, x[13 + 2 * k]); }
}
static double total(const std::vector<double> &x, const std::vector<Log> &logs, double huber, std::vector<double> *per = nullptr) {
  Fit::Shared s; std::vector<Fit::PerLog> pl; decode(x, logs.size(), s, pl);
  double c = 0, w = 0, pen = 0;
  for (size_t k = 0; k < logs.size(); ++k) {
    const double holdW = s.UA * (37.5 - pl[k].Tamb) - s.Q0;                // the heater power that holds 37.5 degC when everything has settled
    if (holdW < 0.0) pen += (-holdW / 160.0) * 0.0004;                     // a room and a fan that would heat the oven past SV by themselves: not this oven
  }
  for (size_t k = 0; k < logs.size(); ++k) {
    double n = 0; const double ck = simulate(s, pl[k], logs[k], nullptr, huber, &n);
    if (per) per->push_back(ck);
    c += ck * n; w += n;
  }
  return w > 0 ? c / w + pen : 1e9;
}

int main(int argc, char **argv) {
  std::vector<Log> logs;
  for (int i = 1; i < argc; ++i) { Log L; if (loadLog(argv[i], L)) logs.push_back(L); else std::fprintf(stderr, "cannot read %s\n", argv[i]); }
  if (logs.empty()) return 1;
  const int restarts = getenv("FIT_RESTARTS") ? atoi(getenv("FIT_RESTARTS")) : 10;
  const int iters = getenv("FIT_ITERS") ? atoi(getenv("FIT_ITERS")) : 3000;
  const double huber = 0.12;
  B = {{2e5, 5e6, true}, {10, 600, true}, {3e5, 8e6, true}, {100, 4000, true}, {30, 250, false}, {5, 90, false}, {0, 4000, false}, {0, 0.6, false}, {60, 900, true}, {0, 120, false}, {0, 1500, false}, {24, 36.0, false}, {-3, 3, false}, {-6, 2, false}};
  const size_t D = 12 + 2 * logs.size();
  std::mt19937_64 rng(12345);
  std::normal_distribution<double> N(0, 1);
  std::uniform_real_distribution<double> U(0, 1);
  std::vector<double> best; double bestC = 1e9;
  if (getenv("FIT_LOAD")) { std::ifstream lf(getenv("FIT_LOAD")); double v; while (lf >> v) best.push_back(v); if (best.size() == D) bestC = total(best, logs, huber); else { best.clear(); std::fprintf(stderr, "FIT_LOAD does not match the logs\n"); } }
  for (int r = 0; r < (best.empty() ? restarts : 0); ++r) {
    std::vector<double> x(D); for (auto &v : x) v = U(rng);
    double c = total(x, logs, huber), sig = 0.25;
    for (int it = 0; it < iters; ++it) {
      std::vector<double> y = x; int nm = 1 + (int)(U(rng) * 3);
      for (int m = 0; m < nm; ++m) { size_t j = (size_t)(U(rng) * D); y[j] = std::max(0.0, std::min(1.0, y[j] + sig * N(rng))); }
      const double cy = total(y, logs, huber);
      if (cy < c) { c = cy; x = y; sig = std::min(0.4, sig * 1.15); } else sig = std::max(0.004, sig * 0.985);
    }
    std::fprintf(stderr, "restart %d: cost %.5f\n", r, c);
    if (c < bestC) { bestC = c; best = x; }
  }
  // polish
  if (!getenv("FIT_LOAD")) { std::vector<double> x = best; double c = bestC, sig = 0.02;
    for (int it = 0; it < iters * 2; ++it) { std::vector<double> y = x; size_t j = (size_t)(U(rng) * D); y[j] = std::max(0.0, std::min(1.0, y[j] + sig * N(rng)));
      const double cy = total(y, logs, huber); if (cy < c) { c = cy; x = y; } else sig = std::max(0.001, sig * 0.998); }
    best = x; bestC = c; }
  if (getenv("FIT_SAVE")) { std::ofstream sf(getenv("FIT_SAVE")); for (double v : best) sf << v << "\n"; }
  Fit::Shared s; std::vector<Fit::PerLog> pl; decode(best, logs.size(), s, pl);
  std::printf("shared: Cf=%.0f UA=%.1f Ce=%.0f hA=%.0f td=%.1f tl=%.1f ventW=%.0f tailFrac=%.2f tailTau=%.0f sensorLag=%.0f Q0=%.0f W (Pmax fixed 16000)   cost(huber)=%.5f\n", s.Cf, s.UA, s.Ce, s.hA, s.td, s.tl, s.ventW, s.tailFrac, s.tailTau, s.sensorLag, s.Q0, bestC);
  std::printf("K = %.2e degC/s per %% heater (air only)   tau_air = Cf/UA = %.0f s\n", 160.0 / s.Cf, s.Cf / s.UA);
  for (size_t k = 0; k < logs.size(); ++k) {
    std::vector<double> err; double n = 0; simulate(s, pl[k], logs[k], &err, huber, &n);
    if (getenv("FIT_DUMP")) { std::printf("  residual (sim - log) every 5 min:"); for (size_t i = 0; i < err.size(); i += 30) std::printf(" %+.2f", err[i]); std::printf("\n"); }
    double ss = 0, mx = 0; for (double e : err) { ss += e * e; mx = std::max(mx, std::fabs(e)); }
    const double hold = (s.UA * (37.5 - pl[k].Tamb) - s.Q0) / 16000.0 * 100.0;
    std::printf("%-34s rows=%4zu  RMS %.3f degC  worst %.2f  Tamb %.1f  mass offset %+.1f  => hold at 37.5 = %.1f %%\n", logs[k].name.c_str(), err.size(), std::sqrt(ss / std::max<size_t>(1, err.size())), mx, pl[k].Tamb, pl[k].d, hold);
  }
    if (getenv("FIT_CLOSED")) {
    // The check that counts: the controller of this build drives the fitted oven in closed loop from the start of each log, and its climb is compared
    // with what the machine really did in that log (the logs of 31/10 afternoon were made by 2.5.5, the others by older builds: indicative).
    std::printf("\nclosed loop (this build on the fitted oven) against the real log:\n");
    std::printf("%-34s | real: reach  peak<=40min  in+-0.1 after 30min | sim: reach  peak<=40min  in+-0.1\n", "log");
    for (size_t k = 0; k < logs.size(); ++k) {
      const Log &L = logs[k];
      auto metrics = [](const std::vector<double> &t, const std::vector<double> &pv, double &reach, double &peak, double &inband) {
        reach = -1; peak = -9; inband = -1;
        size_t ir = pv.size();
        for (size_t i = 0; i < pv.size(); ++i) if (pv[i] >= 37.4) { ir = i; reach = t[i] / 60.0; break; }
        if (ir == pv.size()) return;
        double n = 0, ok = 0;
        for (size_t i = ir; i < pv.size(); ++i) {
          const double dtm = (t[i] - t[ir]) / 60.0;
          if (dtm <= 40.0) peak = std::max(peak, pv[i] - 37.5);
          if (dtm > 30.0) { ++n; if (std::fabs(pv[i] - 37.5) <= 0.1) ++ok; }
        }
        if (n > 0) inband = 100.0 * ok / n;
      };
      double r0, p0, b0; metrics(L.t, L.pv, r0, p0, b0);
      PlantParams pp; pp.Pmax = 16000; pp.Cf = s.Cf; pp.Ce = s.Ce; pp.hA = s.hA; pp.UA = s.UA; pp.td = s.td; pp.tl = s.tl; pp.Tamb = pl[k].Tamb;
      pp.ventW = s.ventW; pp.tailFrac = s.tailFrac; pp.tailTau = s.tailTau; pp.sensorLag = s.sensorLag; pp.Q0 = s.Q0;
      Plant plant(pp, L.pv[0], L.pv[0] + pl[k].d); SensorChain ch(0.1); for (int i = 0; i < 8; ++i) ch.sample(plant.sensor());
      thermalv3::Controller c; thermalv3::Input in; in.sp = 37.5f; in.dQ = 0.1f; in.capPct = 100; in.dtS = 0.1f; in.permit = true;
      std::vector<double> ts, pvs; const double tEnd = L.t.back();
      for (long i = 0; i < (long)(tEnd * 10); ++i) {
        if (i % 20 == 0) ch.sample(plant.sensor());
        in.pv = ch.value();
        plant.step(std::min(100.0, (double)c.step(in).powerPct), false, false);
        if (i % 100 == 0) { ts.push_back(i * 0.1); pvs.push_back(ch.value()); }
      }
      double r1, p1, b1; metrics(ts, pvs, r1, p1, b1);
      std::printf("%-34s |       %5.1f  %+6.2f       %5.0f %%          |      %5.1f  %+6.2f   %5.0f %%\n", L.name.c_str(), r0, p0, b0, r1, p1, b1);
    }
  }
  return 0;
}
