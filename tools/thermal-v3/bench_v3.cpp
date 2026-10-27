// Closed-loop benchmark of the REAL thermal_v3.h (same text the firmware compiles) on the plants of plants.json.
// SIMULATION ONLY: a pass is not proof for a real oven.  Usage: bench_v3 <plants.csv>   (benchmark_v3.py builds it and writes the csv)
// plants.csv columns: id,Pmax,Cf,Ce,hA,UA,td,tl,Tamb,sensorLag
// Plant: heater (transport delay td + first-order lag tl) -> air node (Cf, loss UA to the room) <-> egg/steel node (Ce, exchange hA);
// the sensor sees the air through an optional lag, a 3/8 IIR on a 2 s sample and a 0.1 degC step (the production filter).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "../../MAYAP_INDUSTRIAL_v1_0_0/thermal_v3_adapter.h"

namespace {

struct Plant { std::string id; double Pmax, Cf, Ce, hA, UA, td, tl, Tamb, sensorLag; };
enum Scn { Cold = 0, Warm, Door, Vent, Ambient, ScnN };
const char *const SCN_NAME[ScnN] = {"cold 33C", "warm 36.5C", "door 5min", "vent 5min/30min", "room +4C"};
const double SP = 37.5, EVENT_AT = 2.5 * 3600.0;

struct Result { double peak, reachMin, finalMax, finalMin, inBand, recoverMin, evMin, evMax; };

Result run(const Plant &p, Scn scn) {
  const double dt = 0.1;
  const double T0 = scn == Warm ? 36.5 : 33.0;
  const double hours = scn == Cold || scn == Warm ? 4.0 : 5.0;
  const long steps = static_cast<long>(hours * 36000.0);
  const size_t dl = std::max<size_t>(1, static_cast<size_t>(p.td / dt));
  std::vector<double> pipe(dl, 0.0);
  size_t pi = 0;
  double heater = 0, Ta = T0, Te = T0, sens = T0, iir = T0, u = 0;
  float pv = static_cast<float>(T0);
  ThermalV3Slot slot;
  uint32_t ms = 0;
  Result r{-1, -1, -1e9, 1e9, 0, -1, 1e9, -1e9};
  long inN = 0, finN = 0;
  for (long k = 0; k < steps; ++k) {
    const double t = k * dt;
    double door = 0, vent = 0, amb = p.Tamb;
    if (scn == Door && t >= EVENT_AT && t < EVENT_AT + 300) door = 900.0;                                    // W/K of extra loss
    if (scn == Vent && t >= EVENT_AT && t < EVENT_AT + 3 * 1800 && std::fmod(t - EVENT_AT, 1800.0) < 300) vent = 200.0;
    if (scn == Ambient && t >= EVENT_AT) amb = p.Tamb + 4.0;
    const double pw = u / 100.0 * p.Pmax, d = pipe[pi];
    pipe[pi] = pw; pi = (pi + 1) % dl;
    heater += (d - heater) * dt / std::max(0.1, p.tl);
    const double qe = p.hA * (Ta - Te);
    Ta += (heater - p.UA * (Ta - amb) - qe - (door + vent) * (Ta - amb)) / p.Cf * dt;
    Te += qe / p.Ce * dt;
    sens = p.sensorLag > 0 ? sens + (Ta - sens) * dt / p.sensorLag : Ta;
    if (k % 20 == 0) { iir += (sens - iir) * 0.375; pv = static_cast<float>(std::floor(iir * 10.0 + 0.5) / 10.0); }
    ms += 100;
    ThermalV3SlotIn in;
    in.nowMs = ms; in.pv = pv; in.sp = static_cast<float>(SP); in.newSample = (k % 20 == 0); in.permit = true; in.capPct = 100.0f;
    in.dQ = 0.1f; in.headroomC = 0.7f; in.alarmHeadroomC = 2.0f; in.deliveredPct = static_cast<float>(u);
    u = slot.step(in);
    r.peak = std::max(r.peak, Ta);
    if (r.reachMin < 0 && Ta >= SP - 0.3) r.reachMin = t / 60.0;
    if (scn >= Door && t >= EVENT_AT && t < EVENT_AT + 3600) { r.evMin = std::min(r.evMin, Ta); r.evMax = std::max(r.evMax, Ta); }
    if (scn >= Door && t >= EVENT_AT + 300 && r.recoverMin < 0 && std::fabs(Ta - SP) <= 0.2) r.recoverMin = (t - EVENT_AT - 300) / 60.0;
    if (t >= (hours - 1.0) * 3600 && k % 10 == 0) { ++finN; r.finalMax = std::max(r.finalMax, Ta); r.finalMin = std::min(r.finalMin, Ta); if (std::fabs(Ta - SP) <= 0.1) ++inN; }
  }
  r.inBand = finN ? 100.0 * inN / finN : 0.0;
  return r;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: bench_v3 plants.csv\n"); return 2; }
  std::ifstream f(argv[1]);
  std::vector<Plant> plants;
  std::string line;
  while (std::getline(f, line)) {
    std::stringstream ss(line); std::string x; std::vector<std::string> c;
    while (std::getline(ss, x, ',')) c.push_back(x);
    if (c.size() < 10) continue;
    Plant p{c[0], atof(c[1].c_str()), atof(c[2].c_str()), atof(c[3].c_str()), atof(c[4].c_str()), atof(c[5].c_str()), atof(c[6].c_str()), atof(c[7].c_str()), atof(c[8].c_str()), atof(c[9].c_str())};
    plants.push_back(p);
  }
  // acceptance (simulation): the air never reaches the High line; vortex of the first climb; steady hour; recovery of an event
  const double HIGH_C = 38.2, OVER_COLD = 0.45, OVER_WARM = 0.40, OVER_EVENT = 0.60;
  const double BAND_COLD = 60.0, BAND_WARM = 90.0, BAND_EVENT = 25.0, SPAN_MAX = 0.40, SPAN_EVENT = 0.80, REACH_MAX_MIN = 130.0, RECOVER_MAX_MIN = 15.0;
  int fails = 0;
  std::printf("Thermal V3 closed-loop benchmark (SIMULATION ONLY), %zu plants x %d scenarios\n", plants.size(), static_cast<int>(ScnN));
  for (int s = 0; s < ScnN; ++s) {
    std::printf("\n== %s\nplant   peak  over  reach_min  last-hour: min   max  in+-0.1%%  recover_min  verdict\n", SCN_NAME[s]);
    for (size_t i = 0; i < plants.size(); ++i) {
      const Result r = run(plants[i], static_cast<Scn>(s));
      const double over = r.peak - SP;
      std::string why;
      if (r.peak >= HIGH_C) why += " HIGH";
      const double overMax = s == Cold ? OVER_COLD : (s == Warm ? OVER_WARM : OVER_EVENT);
      if (over > overMax) why += " over";
      if (s <= Warm) {
        if (r.reachMin < 0 || r.reachMin > REACH_MAX_MIN) why += " reach";
        if (r.inBand < (s == Cold ? BAND_COLD : BAND_WARM)) why += " band";
      } else {
        if (r.recoverMin < 0 || r.recoverMin > RECOVER_MAX_MIN) why += " recover";
        if (r.inBand < BAND_EVENT) why += " band";
      }
      if (r.finalMax - r.finalMin > (s <= Warm ? SPAN_MAX : SPAN_EVENT)) why += " span";
      if (!why.empty()) ++fails;
      std::printf("%-6s %6.2f %5.2f %9.1f %15.2f %6.2f %8.0f %11.1f  %s\n", plants[i].id.c_str(), r.peak, over, r.reachMin, r.finalMin, r.finalMax, r.inBand, r.recoverMin, why.empty() ? "ok" : why.c_str());
    }
  }
  std::printf("\n%d case(s) outside the acceptance limits (peak < %.1f, over <= %.2f/%.2f/%.2f, last-hour span <= %.2f, in +-0.1 >= %.0f/%.0f %% (events %.0f %%, span <= %.2f), recovery <= %.0f min)\n",
              fails, HIGH_C, OVER_COLD, OVER_WARM, OVER_EVENT, SPAN_MAX, BAND_COLD, BAND_WARM, BAND_EVENT, SPAN_EVENT, RECOVER_MAX_MIN);
  return fails ? 1 : 0;
}
