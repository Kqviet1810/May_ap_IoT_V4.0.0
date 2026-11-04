// The oven of the field log of 03/11 (firmware 2.9.0): started 0.1 degC under SV, hold about 13 %, a scheduled exhaust-fan run of 3 min every 20 min.
// Measures what the owner saw on the Web: how long the oven takes to close the 0.1 degC gap, how far it dips when the fan runs, and how the PV the
// controller regulates (the fine value of a 0.1 degC reading) spikes. The plant is the oven fitted to the 29/10 log (tests/thermal-sim.h) with UA set so
// that the hold at 37.5 degC is 13 % and the fan takes about 7 % (the field log: -0.1 degC in 2.7 min at a request of hold + 0.5 %).
//   g++ -O2 -std=c++11 -I MAYAP_INDUSTRIAL_v1_0_0 -I tests tools/thermal-research/log-oven.cpp -o /tmp/log-oven && /tmp/log-oven
// Environment: TD (110), TL (80), SEEDS (10), MIN (60), START (37.40), KAIR (0: leave the controller's own), NOISE (0.025), FANW (1500), UA (220).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <algorithm>
#include "sensor_fine.h"
#include "thermal_v3.h"
#include "thermal-sim.h"

static double gauss(uint32_t &x) {
  auto u = [&x]() { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return (x + 1.0) / 4294967297.0; };
  return std::sqrt(-2.0 * std::log(u())) * std::cos(6.283185307179586 * u());
}

struct Res { std::vector<double> dips; double tReach = -1, errMid = 0, errLate = 0, sdLate = 0, fanDip = 0, fanDip2 = 0, spike = 0, reqSd = 0, over = 0; };

static Res run(uint32_t seed) {
  using namespace thermalsim;
  const double sp = 37.5, start = getenv("START") ? atof(getenv("START")) : 37.40, noise = getenv("NOISE") ? atof(getenv("NOISE")) : 0.025;
  const double tEnd = 60.0 * (getenv("MIN") ? atof(getenv("MIN")) : 60.0);
  PlantParams pp = u29Oven();
  if (getenv("TD")) pp.td = atof(getenv("TD"));
  if (getenv("TL")) pp.tl = atof(getenv("TL"));            // TD / TL: the delay and the lag of the plant (the field says the real transit time is 50..130 s)
  pp.UA = getenv("UA") ? atof(getenv("UA")) : 220.0; pp.ventW = getenv("FANW") ? atof(getenv("FANW")) : 1500.0;
  Plant pl(pp, start, start); pl.settleAt(start);
  thermalv3::Controller c;
  if (getenv("KAIR")) c.restore((float)atof(getenv("KAIR")), getenv("TAU") ? (float)atof(getenv("TAU")) : 0.0f, getenv("FANLOSS") ? (float)atof(getenv("FANLOSS")) : 0.0f);   // KAIR=9.4e-5: the numbers of this oven were kept across the restart
  sensorfine::Estimator fv, fs; sensorfine::Params pv, ps; pv.minWindowS = getenv("VMIN") ? (float)atof(getenv("VMIN")) : 10.0f; pv.maxWindowS = getenv("VMAX") ? (float)atof(getenv("VMAX")) : 30.0f;   // the window of the fine value the controller regulates
  uint32_t rng = seed * 2654435761u + 12345u;
  std::vector<double> airT, reqT, pvT; double qh[3] = {start, start, start};
  double lastU = 0; Res r; float pvFine = (float)start;
  const double fanStart = 1670.0, fanLen = 160.0, fanEvery = 1200.0;
  for (long i = 0; i < (long)(tEnd * 10); ++i) {
    const double t = i * 0.1;
    if (i % 10 == 0) {                                                       // one reading a second (the machine polls every 1 s)
      const double q = std::lround((pl.sensor() + noise * gauss(rng)) * 10.0) / 10.0;
      qh[2] = qh[1]; qh[1] = qh[0]; qh[0] = q;
      double qv = q; if (getenv("MED3")) qv = std::max(std::min(qh[0], qh[1]), std::min(std::max(qh[0], qh[1]), qh[2]));   // MED3: the median of the last three readings feeds the value estimate
      fv.update((float)qv, (float)t, 0.1f, pv); fs.update((float)q, (float)t, 0.1f, ps);
    }
    if (fv.ready()) pvFine = fv.valueAt((float)(t - std::floor(t)));
    const bool fan = t >= fanStart && std::fmod(t - fanStart, fanEvery) < fanLen;
    thermalv3::Input in; in.pv = pvFine; in.sp = (float)sp; in.dtS = 0.1f; in.permit = true; in.capPct = 100.0f; in.ventFan = fan;
    in.slopeFine = fs.ready() ? fs.slopePerS() : NAN;
    if (!getenv("NOSCHED")) { const double ph = t < fanStart ? -1.0 : std::fmod(t - fanStart, fanEvery); if (t < fanStart) { in.fanOnInS = (float)(fanStart - t); in.fanOffInS = (float)(fanStart - t + fanLen); } else if (ph < fanLen) { in.fanOnInS = 0; in.fanOffInS = (float)(fanLen - ph); } else { in.fanOnInS = (float)(fanEvery - ph); in.fanOffInS = (float)(fanEvery - ph + fanLen); } }
    const float u = c.step(in).powerPct; lastU = u;
    pl.step(u, fan, false);
    if (i % 10 == 0) { airT.push_back(pl.air()); reqT.push_back(u); pvT.push_back(pvFine); }
  }
  for (size_t k = 0; k < airT.size(); ++k) if (r.tReach < 0 && std::fabs(airT[k] - sp) <= 0.05) r.tReach = (double)k;
  auto mean = [&](size_t a, size_t b, const std::vector<double> &v) { double s = 0; size_t n = 0; for (size_t k = a; k < std::min(b, v.size()); ++k) { s += v[k]; ++n; } return n ? s / n : 0.0; };
  r.errMid = mean(600, 1500, airT) - sp; r.errLate = mean(2400, airT.size(), airT) - sp;
  { double m = mean(2400, airT.size(), airT), s = 0; size_t n = 0; for (size_t k = 2400; k < airT.size(); ++k) { s += (airT[k] - m) * (airT[k] - m); ++n; } r.sdLate = n ? std::sqrt(s / n) : 0; }
  // fan dip: lowest air inside the fan runs against the air just before the run
  for (int f = 0; f < 12; ++f) {
    const size_t a = (size_t)(fanStart + f * fanEvery), b = a + (size_t)fanLen + 120;
    if (b >= airT.size()) break;
    double before = airT[a], mn = 1e9; for (size_t k = a; k < b; ++k) mn = std::min(mn, airT[k]);
    r.dips.push_back(before - mn);
    (f == 0 ? r.fanDip : r.fanDip2) = before - mn;
  }
  { double s = 0; size_t n = 0; for (size_t k = 2400; k + 1 < pvT.size(); ++k) { s += std::fabs(pvT[k + 1] - pvT[k]); ++n; } r.spike = n ? s / n : 0; }
  { double m = mean(2400, reqT.size(), reqT), s = 0; size_t n = 0; for (size_t k = 2400; k < reqT.size(); ++k) { s += (reqT[k] - m) * (reqT[k] - m); ++n; } r.reqSd = n ? std::sqrt(s / n) : 0; }
  for (double v : airT) r.over = std::max(r.over, v - sp);
  (void)lastU;
  return r;
}

struct Agg { double reach = 99, dip4 = 9, late = 9, over = 0, dipLast = 9; int reached = 0, n = 0; std::vector<double> dips; };
static Agg aggregate(int n) {
  Res a; std::vector<double> dsum; Agg g; g.n = n; double tReach = 0;
  for (int s = 1; s <= n; ++s) {
    Res r = run((uint32_t)s);
    if (r.tReach >= 0) { tReach += r.tReach; ++g.reached; }
    if (dsum.size() < r.dips.size()) dsum.resize(r.dips.size(), 0.0);
    for (size_t k = 0; k < r.dips.size(); ++k) dsum[k] += r.dips[k];
    a.errLate += r.errLate; a.over = std::max(a.over, r.over);
  }
  g.reach = g.reached ? tReach / g.reached / 60.0 : 99.0; g.late = a.errLate / n; g.over = a.over;
  for (double d : dsum) g.dips.push_back(d / n);
  g.dip4 = g.dips.size() > 3 ? g.dips[3] : 9.0; g.dipLast = g.dips.empty() ? 9.0 : g.dips.back();
  return g;
}

int main(int argc, char **argv) {
  const bool gate = argc > 1 && !std::strcmp(argv[1], "--gate");
  if (gate) {
    // 1. 2.9.1: the oven's numbers were kept across the restart (started 0.1 degC under SV on the 03/11 oven). TAU = what a climb measures on this plant (the stopwatch
    //    reads 222 s; learn run of 2.9.2), so the feed-forward leads by that. A longer belief than the oven needs is slower to close the gap than the prior is.
    setenv("KAIR", "9.4e-5", 1); setenv("TAU", "222", 1); setenv("MIN", "120", 1);
    Agg k = aggregate(10);
    const bool ok1 = k.reached == k.n && k.reach <= 18.0 && k.dip4 <= 0.06 && k.over <= 0.12 && std::fabs(k.late) <= 0.05;
    std::printf("log-oven gate, numbers kept: reach %.1f min (<= 18; 2.9.1: 19.2 on the same numbers), 4th fan dip %.3f (<= 0.06), over SV %+.3f (<= 0.12; 2.9.1: +0.138), late error %+.3f (|.| <= 0.05): %s\n", k.reach, k.dip4, k.over, k.late, ok1 ? "OK" : "FAIL");
    // 2. 2.9.2: an oven that has never measured itself (no climb since the numbers are kept: the field logs of 03/11 and 04/11 had identified = 0 on every line), over the
    //    whole range of transit times the field gives (td + tl 60 .. 130 s): the priors are the machine's own, so the gap closes in minutes, the fan feed-forward with
    //    the prior transit time never overshoots by more than 0.08 degC and the dip of the fan is learned down to 0.06 degC.
    unsetenv("KAIR"); unsetenv("TAU");
    const double plants[4][2] = {{30, 30}, {50, 40}, {80, 50}, {110, 80}};
    bool ok2 = true;
    for (int p = 0; p < 4; ++p) {
      char td[16], tl[16]; std::snprintf(td, sizeof td, "%.0f", plants[p][0]); std::snprintf(tl, sizeof tl, "%.0f", plants[p][1]);
      setenv("TD", td, 1); setenv("TL", tl, 1);
      Agg u = aggregate(10);
      const bool slowest = p == 3;                                           // td + tl = 190 s is beyond the 50..130 s of the field: the feed-forward is only asked not to be worse than none
      const bool ok = u.reached == u.n && u.reach <= 10.0 && u.over <= (slowest ? 0.16 : 0.08) && u.dipLast <= (slowest ? 0.11 : 0.06) && std::fabs(u.late) <= 0.05;
      ok2 = ok2 && ok;
      std::printf("log-oven gate, never measured, td %.0f tl %.0f: reach %.1f min (<= 10), last fan dip %.3f (<= %.2f), over SV %+.3f (<= %.2f), late error %+.3f: %s\n", plants[p][0], plants[p][1], u.reach, u.dipLast, slowest ? 0.11 : 0.06, u.over, slowest ? 0.16 : 0.08, u.late, ok ? "OK" : "FAIL");
    }
    return ok1 && ok2 ? 0 : 1;
  }
  const int n = getenv("SEEDS") ? atoi(getenv("SEEDS")) : 10;
  Res a; int reached = 0; std::vector<double> dsum;
  for (int s = 1; s <= n; ++s) {
    Res r = run((uint32_t)s);
    if (r.tReach >= 0) { a.tReach += r.tReach; ++reached; }
    if (dsum.size() < r.dips.size()) dsum.resize(r.dips.size(), 0.0);
    for (size_t k = 0; k < r.dips.size(); ++k) dsum[k] += r.dips[k];
    a.errMid += r.errMid; a.errLate += r.errLate; a.sdLate += r.sdLate; a.fanDip += r.fanDip; a.fanDip2 += r.fanDip2; a.spike += r.spike; a.reqSd += r.reqSd; a.over = std::max(a.over, r.over);
  }
  std::printf("seeds %d | time to within 0.05 of SV: %.1f min (%d/%d reached) | true error min10-25 %+.3f, last %+.3f (sd %.3f) | fan dip 1st %.3f 2nd %.3f degC | mean |dPV|/s %.4f | request sd %.2f %% | max over SV %+.3f\n",
              n, reached ? a.tReach / reached / 60.0 : -1.0, reached, n, a.errMid / n, a.errLate / n, a.sdLate / n, a.fanDip / n, a.fanDip2 / n, a.spike / n, a.reqSd / n, a.over);
  std::printf("  dip per fan run:"); for (double d : dsum) std::printf(" %.3f", d / n); std::printf("\n");
  return 0;
}
