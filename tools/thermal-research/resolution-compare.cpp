// Compares a 0.1 degC and a 0.01 degC sensor on the first climb and 30 minutes of holding, over 53 simulated ovens (every 4th of the fleet bench).
// Build: g++ -O2 -std=c++17 -DHDR=\"../../MAYAP_INDUSTRIAL_v1_0_0/thermal_v3.h\" tools/thermal-research/resolution-compare.cpp -o /tmp/res   Run: /tmp/res 0.03   (argument = sensor noise sigma in degC)
#ifndef HDR
#define HDR "../../MAYAP_INDUSTRIAL_v1_0_0/thermal_v3.h"
#endif
#include HDR
#include "../../tests/thermal-sim.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <random>
using namespace thermalsim;
struct M { double reach=-1, peak=0, climbCV=0, uTV=0, rms=0, pp=0, uStd=0; };
static M run(const PlantParams &p, double step, double noise, unsigned seed) {
  Plant pl(p, 31.4, 31.2); SensorChain ch(step); std::mt19937 rng(seed); std::normal_distribution<double> nd(0.0, 1.0);
  for (int i = 0; i < 8; ++i) ch.sample(pl.sensor());
  thermalv3::Controller c; thermalv3::Input in; in.sp = 37.5f; in.dQ = (float)step; in.capPct = 100; in.dtS = 0.1f; in.permit = true;
  M m; std::vector<double> airMin, uMin; double umean = 0, aacc = 0; long ucount = 0; double uacc = 0; int tick = 0;
  std::vector<double> u60, a60; const double T = 9000;
  double lastU = 0;
  for (long i = 0; i < (long)(T * 10); ++i) {
    double t = i * 0.1;
    if (i % 20 == 0) ch.sample(pl.sensor() + noise * nd(rng));
    in.pv = ch.value();
    lastU = c.step(in).powerPct;
    pl.step(std::min(100.0, lastU), false, false);
    uacc += lastU; aacc += pl.air(); ++tick;
    if (tick == 600) { u60.push_back(uacc / 600.0); a60.push_back(aacc / 600.0); uacc = aacc = 0; tick = 0; }
    m.peak = std::max(m.peak, pl.air());
    if (m.reach < 0 && pl.air() >= 37.4) m.reach = t;
  }
  // climb straightness: 60 s slopes while the true air is between 33.0 and 36.5
  std::vector<double> sl; for (size_t k = 1; k < a60.size(); ++k) if (a60[k-1] > 33.0 && a60[k] < 36.5) sl.push_back(a60[k] - a60[k-1]);
  if (sl.size() >= 3) { double mean = 0; for (double s : sl) mean += s; mean /= sl.size(); double var = 0; for (double s : sl) var += (s - mean) * (s - mean); var /= sl.size(); m.climbCV = mean > 1e-9 ? std::sqrt(var) / mean : 0; }
  // heater roughness: mean |change| of 60 s mean power after the climb (last 30 min), steady error
  size_t from = a60.size() > 30 ? a60.size() - 30 : 0; double tv = 0, e2 = 0, lo = 99, hi = 0, um = 0; int n = 0;
  for (size_t k = from; k < a60.size(); ++k) { if (k > from) tv += std::fabs(u60[k] - u60[k-1]); e2 += (a60[k] - 37.5) * (a60[k] - 37.5); lo = std::min(lo, a60[k]); hi = std::max(hi, a60[k]); um += u60[k]; ++n; }
  m.uTV = tv / std::max(1, n - 1); m.rms = std::sqrt(e2 / n); m.pp = hi - lo; double us = 0; um /= n; for (size_t k = from; k < a60.size(); ++k) us += (u60[k] - um) * (u60[k] - um); m.uStd = std::sqrt(us / n);
  return m;
}
int main(int argc, char **argv) {
  std::vector<PlantParams> ov; for (auto p : family(11, 150, false)) ov.push_back(p); for (auto p : family(23, 60, true)) ov.push_back(p);
  const double noise = argc > 1 ? atof(argv[1]) : 0.0;
  for (double step : {0.1, 0.01}) {
    std::vector<M> r; for (size_t k = 0; k < ov.size(); k += 4) r.push_back(run(ov[k], step, noise, 1234u + (unsigned)k));
    auto med = [&](double M::*f) { std::vector<double> v; for (auto &x : r) v.push_back(x.*f); std::sort(v.begin(), v.end()); return std::make_pair(v[v.size()/2], v[(size_t)(0.9*v.size())]); };
    auto R = med(&M::reach), P = med(&M::peak), C = med(&M::climbCV), T = med(&M::uTV), S = med(&M::rms), Q = med(&M::pp), U = med(&M::uStd);
    std::printf("step %.2f noise %.2f n=%zu | reach med %.0f s p90 %.0f | peak med %.2f p90 %.2f | climb slope CV med %.2f p90 %.2f | steady rms med %.3f p90 %.3f | air p-p med %.2f p90 %.2f | heater change/min med %.2f p90 %.2f | heater std med %.2f p90 %.2f\n", step, noise, r.size(), R.first, R.second, P.first, P.second, C.first, C.second, S.first, S.second, Q.first, Q.second, T.first, T.second, U.first, U.second);
  }
}
