// Pass rates of the thermal controller on a family of simulated ovens: the yardstick every change of thermal_v3.h has to keep or raise.
// Build: g++ -O2 -std=c++17 -DTHERMAL_SENSOR_NOISE_K=0.02 tools/thermal-research/pass-rates.cpp -o /tmp/pass-rates      Run: /tmp/pass-rates [ABCDE]   (about 2 minutes)
// Ovens: 10 slow ones + 10 others of the closed-loop family + the oven of the 29/10 batch, in the rooms where each can hold SV. Noise on the reading: THERMAL_SENSOR_NOISE_K degC.
//   A  the scheduled fan (7.2 min in every 40), 16 runs after a climb, rooms 5/15/25/35: the worst dip and rise of the last 4 runs, E115 must not trip
//   B  24 h of weather (room 20 +- 10 degC) with the fan: the share of the time the air is inside the band (true air temperature, 2 h after the start)
//   C  the first climb from 25 degC (rooms 8/20/30): the overshoot, and the last 30 minutes inside +-0.15 degC
//   D  as B without the fan
//   E  a restart next to SV (kAir, tau and 0.9 x the hold restored as the machine does, the compensation of the fan restored or not, the fan 16 minutes later), rooms 12/22/30:
//      the error before the fan, the first and the second run of the fan
// Every group is printed for the bands +-0.05 / +-0.1 / +-0.2 degC (C: overshoot 0.1 / 0.2 / 0.3 / 0.5). The band of the owner is +-0.05.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "../../tests/thermal-scenarios.h"
using namespace thermalsim;

template <class I> static auto setFan(I &i, float a, float b, int) -> decltype(i.fanOnInS, void()) { i.fanOnInS = a; i.fanOffInS = b; }
template <class I> static void setFan(I &, float, float, long) {}
template <class C> static auto restoreFan(C &c, const C &o, int) -> decltype(c.restoreFan(o.fanComp(), o.fanLeadS()), void()) { c.restoreFan(o.fanComp(), o.fanLeadS()); }
template <class C> static void restoreFan(C &, const C &, long) {}

static const double PERIOD = 2400.0, ON = 432.0, DAY = 86400.0;
static const double BANDS[3] = {0.05, 0.10, 0.20};
static const double OVERS[4] = {0.10, 0.20, 0.30, 0.50};

static std::vector<PlantParams> ovens() {
  auto v = family(4343u, 10, true); auto w = family(4242u, 10, false); v.insert(v.end(), w.begin(), w.end()); v.push_back(u29Oven());
  return v;
}
static bool feasible(const PlantParams &p, double ta) { return (p.UA + p.ventW / 12.5) * (SP_C - ta) / p.Pmax <= 0.85; }
static void fanClock(Rig &r, double base, double &ph, bool &vent) {
  ph = std::fmod(r.now() - base, PERIOD); vent = ph < ON;
  setFan(r.in, vent ? 0.0f : static_cast<float>(PERIOD - ph), vent ? static_cast<float>(ON - ph) : static_cast<float>(NAN), 0);
}
static void line(const char *name, const int *ok, int n, const char *unit) {
  std::printf("%-46s", name);
  for (int i = 0; i < 3; ++i) std::printf("  %s%.2f: %2d/%2d = %3.0f %%", unit, BANDS[i], ok[i], n, n ? 100.0 * ok[i] / n : 0.0);
  std::printf("\n");
}

int main(int argc, char **argv) {
  const std::string only = argc > 1 ? argv[1] : "ABCDE";
  const bool want[5] = {only.find('A') != std::string::npos, only.find('B') != std::string::npos, only.find('C') != std::string::npos,
                        only.find('D') != std::string::npos, only.find('E') != std::string::npos};
  const std::vector<PlantParams> fleet = ovens();
  if (want[0]) {
    int ok[3] = {0, 0, 0}, n = 0;
    for (double ta : {5.0, 15.0, 25.0, 35.0}) for (const PlantParams &q : fleet) {
      PlantParams p = q; p.Tamb = ta; if (!feasible(p, ta)) continue;
      Rig r(p, 25.0, 25.0); r.advance(5 * 3600.0);
      const double base = r.now(); double wd = 0, wo = 0;
      for (int c = 0; c < 16; ++c) {
        double mn = 99, mx = -99;
        for (int s = 0; s < static_cast<int>(PERIOD); ++s) { double ph; bool vent; fanClock(r, base, ph, vent); r.advance(1.0, false, vent); mn = std::min(mn, r.plant.air()); mx = std::max(mx, r.plant.air()); }
        if (c >= 12) { wd = std::max(wd, SP_C - mn); wo = std::max(wo, mx - SP_C); }
      }
      ++n; for (int i = 0; i < 3; ++i) ok[i] += (!r.tripped() && wd <= BANDS[i] && wo <= BANDS[i]);
    }
    line("A  every fan run: dip and rise within", ok, n, "+-");
  }
  for (int fan = 1; fan >= 0; --fan) {
    if (!want[fan ? 1 : 3]) continue;
    int ok[3] = {0, 0, 0}, n = 0; double mean[3] = {0, 0, 0};
    for (const PlantParams &q : fleet) {
      PlantParams p = q; p.Tamb = 10; if (!feasible(p, 10)) continue;
      p.Tamb = 20;
      Rig r(p, 25.0, 25.0); r.advance(5 * 3600.0);
      const double base = r.now(); long inBand[3] = {0, 0, 0}, cnt = 0;
      for (long s = 0; s < static_cast<long>(DAY); ++s) {
        const double t = r.now() - base;
        r.plant.setAmbient(20 + 10 * std::sin(6.2831853 * t / DAY));
        double ph = 0; bool vent = false;
        if (fan) fanClock(r, base, ph, vent); else setFan(r.in, static_cast<float>(NAN), static_cast<float>(NAN), 0);
        r.advance(1.0, false, vent);
        if (t > 7200) { ++cnt; const double d = std::fabs(r.plant.air() - SP_C); for (int i = 0; i < 3; ++i) inBand[i] += d <= BANDS[i]; }
      }
      ++n; for (int i = 0; i < 3; ++i) { const double f = static_cast<double>(inBand[i]) / cnt; mean[i] += f; ok[i] += (!r.tripped() && f >= 0.90); }
    }
    line(fan ? "B  24 h weather + fan: >= 90 % of the time within" : "D  24 h weather, no fan: >= 90 % of the time within", ok, n, "+-");
    std::printf("%-46s  mean share of the time inside the band: %.0f %% / %.0f %% / %.0f %%\n", "", 100 * mean[0] / n, 100 * mean[1] / n, 100 * mean[2] / n);
  }
  if (want[2]) {
    int ok[4] = {0, 0, 0, 0}, n = 0;
    for (double ta : {8.0, 20.0, 30.0}) for (const PlantParams &q : fleet) {
      PlantParams p = q; p.Tamb = ta; if (!feasible(p, ta)) continue;
      Rig r(p, 25.0, 25.0); double mx = -99;
      for (int s = 0; s < 4 * 3600; ++s) { r.advance(1.0); mx = std::max(mx, r.plant.air() - SP_C); }
      double fin = 0; for (int s = 0; s < 1800; ++s) { r.advance(1.0); fin = std::max(fin, std::fabs(r.plant.air() - SP_C)); }
      ++n; for (int i = 0; i < 4; ++i) ok[i] += (!r.tripped() && mx <= OVERS[i] && fin <= 0.15);
    }
    std::printf("%-46s  over<=%.2f: %2d/%2d = %3.0f %%  over<=%.2f: %3.0f %%  over<=%.2f: %3.0f %%  over<=%.2f: %3.0f %%\n", "C  first climb (last 30 min within +-0.15)", OVERS[0], ok[0], n, 100.0 * ok[0] / n,
                OVERS[1], 100.0 * ok[1] / n, OVERS[2], 100.0 * ok[2] / n, OVERS[3], 100.0 * ok[3] / n);
  }
  if (want[4]) {
    for (int carry = 0; carry < 2; ++carry) {
      int okSteady[3] = {0, 0, 0}, okFirst[3] = {0, 0, 0}, okBoth[3] = {0, 0, 0}, n = 0; double dip1 = 0;
      for (double ta : {12.0, 22.0, 30.0}) for (const PlantParams &q : fleet) {
        PlantParams p = q; p.Tamb = ta; if (!feasible(p, ta)) continue;
        Rig r(p, 25.0, 25.0); r.advance(5 * 3600.0);
        const double base = r.now();
        auto runTo = [&](double tEnd) { while (r.now() - base < tEnd - 1e-9) { double ph; bool vent; fanClock(r, base, ph, vent); r.advance(1.0, false, vent); } };
        runTo(12 * PERIOD + (PERIOD - 1000.0));
        const thermalv3::Controller old = r.ctl;
        const float kA = r.ctl.kAir(), tau = r.ctl.tauTransit(), hold = r.ctl.pHold();
        r.restartControllers(); r.ctl.restore(kA, tau); r.ctl.restoreHold(0.9f * hold);
        if (carry) restoreFan(r.ctl, old, 0);
        r.skipClock(45.0);
        const double t0 = r.now() - base;
        runTo(t0 + 3 * PERIOD);
        const auto &a = r.air(); const size_t n0 = a.size() - static_cast<size_t>(3 * PERIOD);
        double sSum = 0, sN = 0, mn1 = 99, mx1 = -99, mn2 = 99, mx2 = -99;
        for (size_t i = n0; i < a.size(); ++i) {
          const double s = static_cast<double>(i - n0);
          if (s >= 300 && s < 1000) { sSum += SP_C - a[i]; ++sN; }
          if (s >= 1000 && s < 1000 + ON + 600) { mn1 = std::min(mn1, static_cast<double>(a[i])); mx1 = std::max(mx1, static_cast<double>(a[i])); }
          if (s >= 1000 + PERIOD && s < 1000 + PERIOD + ON + 600) { mn2 = std::min(mn2, static_cast<double>(a[i])); mx2 = std::max(mx2, static_cast<double>(a[i])); }
        }
        const double st = std::fabs(sN ? sSum / sN : 0), d1 = SP_C - mn1, o1 = mx1 - SP_C, d2 = SP_C - mn2, o2 = mx2 - SP_C;
        ++n; dip1 += d1;
        for (int i = 0; i < 3; ++i) {
          okSteady[i] += st <= BANDS[i]; const bool f1 = d1 <= BANDS[i] && o1 <= BANDS[i], f2 = d2 <= BANDS[i] && o2 <= BANDS[i];
          okFirst[i] += f1; okBoth[i] += (f1 && f2 && !r.tripped());
        }
      }
      std::printf("E  restart, compensation of the fan %s (mean first dip %.2f degC)\n", carry ? "RESTORED" : "lost    ", dip1 / n);
      line("     error before the fan inside", okSteady, n, "+-");
      line("     first fan run inside", okFirst, n, "+-");
      line("     first and second run inside", okBoth, n, "+-");
    }
  }
  return 0;
}
