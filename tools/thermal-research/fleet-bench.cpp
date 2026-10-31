// Fleet bench of the thermal controller: 210 random ovens x 8 situations, with the machine's REAL fan line (vent_line.h, the class MachineController
// runs; FAN=v250 runs the rule of 2.5.0, written out here, for the comparison), a small or a strong exhaust fan.
// Build: g++ -O2 -std=c++17 tools/thermal-research/fleet-bench.cpp -o /tmp/fleet-bench
// Run:   /tmp/fleet-bench 100   (fan of 100 W; 2500 for a strong one; a second argument is a bit mask of the situations, 1023 = all)
// One line per run: oven, situation, seconds to 37.4, peak, lowest, highest, minutes back within +-0.1, integral |error| [degC min], rms before the
// event, hold %, delay s, minutes over SP+0.1 (total, longest run), minutes to settle within +-0.2, fan minutes, fan starts, lowest hold level.
// Situations: 0 climb and hold, 1 scheduled fan, 2 one step over SP, 3 door 3 min, 4 door 4.5 min + cold tray, 5 reboot (45 s without power),
//   6 the room warms up 7 degC in 2 h (the hold the oven needs falls to a few percent), 7 the day profile of the last week (28 of every 40 min the fans run),
//   8 reboot (45 s) that takes back what a stable run had stored an hour earlier (oven constants + hold level, thermal_learn.h),
//   9 the batch is cancelled for 2 min and started again at once (the controller keeps what it learned, nothing is stored).
#ifndef HDR
#define HDR "../../MAYAP_INDUSTRIAL_v1_0_0/thermal_v3.h"
#endif
#include HDR
#include "../../MAYAP_INDUSTRIAL_v1_0_0/vent_line.h"
#include "../../tests/thermal-sim.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
using namespace thermalsim;
struct Res { double highs = 0, reach = -1, peak = 0, minAfter = 99, maxAfter = 0, rec = 0, iae = 0, rmsPre = 0, tOver = 0, tOver10 = 0, settle2 = 0, fanStarts = 0, fanOnMin = 0, lowestHold = 100; };
static Res run(const PlantParams &p, int scen) {
  const double tEv = 9000, tEnd = (scen == 6 || scen == 7) ? 9000 + 9000 : 15000;
  const bool oldFan = getenv("FAN") && !strcmp(getenv("FAN"), "v250");
  VentLine vl; VentLine::Params vp; vp.stepC = 0.1f; double heaterAvg = 0.0; bool vlOn = false;
  const double tamb0 = p.Tamb; float lastU = 0.0f; bool fanPrev = false;
  Plant pl(p, 31.4, 31.2); SensorChain ch(0.1); for (int i = 0; i < 8; ++i) ch.sample(pl.sensor());
  thermalv3::Controller c; thermalv3::Input in; in.sp = 37.5f; in.dQ = 0.1f; in.capPct = 100; in.dtS = 0.1f; in.permit = true;
  bool vline = false, block = false; double aboveSince = -1, abovePv = 0, offT = 0, fastT = 0, notBefore = 0; std::vector<double> starts;
  struct Snap { float kAir = 0, tau = 0, hold = 0; uint16_t pipe = 0; bool have = false; } snap;
  const float highAt = getenv("HIGHCUT") ? (float)atof(getenv("HIGHCUT")) : 0.0f;   // HIGHCUT=37.7: the High line of the field machine cuts the heater (permit lost, fans forced) until the reading is 0.15 lower and 4 min passed
  bool hc = false; double hcT = 0;
  bool inserted = false, rebooted = false; Res r; double sqPre = 0; long nPre = 0, lastOut = -1, lastOut2 = -1; double overRun = 0;
  for (long i = 0; i < (long)(tEnd * 10); ++i) {
    double t = i * 0.1;
    if (i % 20 == 0) ch.sample(pl.sensor());
    float pv = ch.value(); in.pv = pv;
    // the day profile of the exhaust fans (scheduled fan of situation 1; the heavy profile of situation 7): is it running, and when does it start
    bool sched = false; double profIn = NAN;
    if (scen == 1 && t > 8400) { double ph = fmod(t, 1200.0); sched = ph < 180.0; profIn = sched ? 0.0 : 1200.0 - ph; }
    if (scen == 7 && t >= tEv) { double ph = fmod(t - tEv, 2400.0); sched = ph < 1680.0; profIn = sched ? 0.0 : 2400.0 - ph; }
    const float onT = getenv("ONT") ? (float)atof(getenv("ONT")) : 37.6f;   // the vent threshold of the field machine: one step over SP
    if (oldFan) {
      // the rule of 2.5.0: a whole minute over the threshold without a dip, or 0.1 more for 20 s; 3 min on / 5 min off, at most 6 starts an hour
      bool onPast = pv >= onT;
      if (!onPast) { aboveSince = -1; } else if (aboveSince < 0) { aboveSince = t; abovePv = pv; } else if (pv < abovePv - 0.05f) { aboveSince = t; abovePv = pv; }
      bool trend = onPast && aboveSince >= 0 && t - aboveSince >= 60;
      if (!vline && pv >= onT + 0.1f) fastT += 0.1; else fastT = 0;
      bool offPast = pv <= 37.505f; if (vline && offPast) offT += 0.1; else offT = 0;
      int nh = 0; for (double s2 : starts) if (t - s2 < 3600) ++nh;
      if (!vline && ((trend || fastT >= 20) && t >= notBefore && nh < 6)) { vline = true; notBefore = t + 180; starts.push_back(t); r.fanStarts += 1; }
      else if (vline && offT >= 20 && t >= notBefore) { vline = false; notBefore = t + 300; }
    } else {
      heaterAvg += ((double)lastU - heaterAvg) * 0.1 / 60.0;
      VentLine::Ctx vc; vc.tauS = c.identified() ? c.tauTransit() : NAN; vc.heaterAvgPct = (float)heaterAvg; vc.predictedC = c.predicted();
      vc.fanRunning = sched; vc.profileOnInS = (profIn > 0.0) ? (float)profIn : NAN;
      vl.update((uint32_t)(t * 1000.0), true, pv, onT, 37.5f, vp, vc);
      bool nowOn = vl.active();
      if (nowOn && !vlOn && !sched) r.fanStarts += 1;                       // a contactor operation: the fans were stopped
      vlOn = nowOn; vline = nowOn;
    }
    if (getenv("FANLOG") && vline != fanPrev) { std::printf("   t=%6.0f s  fan %s  pv=%.2f u=%.1f hold=%.1f why=%d sched=%d\n", t, vline ? "ON " : "off", pv, lastU, c.pHold(), (int)vl.why(), sched ? 1 : 0); }
    fanPrev = vline;
    if (vline) r.fanOnMin += 0.1 / 60.0;
    if (getenv("NOBLOCK")) block = false; else if (!block && pv >= 37.8f) block = true; else if (block && pv <= 37.7f) block = false;
    if (highAt > 0.0f) { if (!hc && pv >= highAt) { hc = true; hcT = t; r.highs += 1; } else if (hc && pv <= highAt - 0.15f && t - hcT >= 240.0) hc = false; }
    bool fan = vline || sched || hc;
    bool door = (scen == 3 && t >= tEv && t < tEv + 180) || (scen == 4 && t >= tEv && t < tEv + 270);
    if (scen == 4 && !inserted && t >= tEv + 270) { pl.insertColdMass(1.1, 22.0); inserted = true; }
    if (scen == 8 && !snap.have && t >= tEv - 3600) { snap.kAir = c.kAir(); snap.tau = c.tauTransit(); snap.hold = c.pHold(); snap.pipe = c.pipeLen(); snap.have = true; }
    bool down = ((scen == 5 || scen == 8) && t >= tEv && t < tEv + 45);
    if ((scen == 5 || scen == 8) && !rebooted && t >= tEv + 45) {
      c.reset();
      if (scen == 8 && snap.have) { c.importOven(snap.kAir, snap.tau, snap.pipe); c.importHold(snap.hold, true); }   // what the last stable run stored
      rebooted = true;
    }
    const bool paused = scen == 9 && t >= tEv && t < tEv + 120;                  // batch cancelled, started again at once: the controller is frozen, not reset
    in.shadow = block && !hc; in.permit = !block && !paused && !hc;
    if (scen == 6 && t >= tEv) pl.setAmbient(tamb0 + (getenv("AMB") ? atof(getenv("AMB")) : 7.0) * std::min(1.0, (t - tEv) / 7200.0));   // AMB=-5: the room cools instead
    float u = 0; if (!down) { auto o = c.step(in); u = (block || hc) ? 0.0f : o.powerPct; }
    lastU = u;
    double extra = (scen == 2 && t >= tEv && t < tEv + 150) ? 12.0 : 0.0;
    pl.step(std::min(100.0, (double)u + extra), fan, door);
    if (getenv("DUMP") && i % 300 == 0 && t >= 0 && t <= 12000) std::printf("t=%6.0f pv=%.2f u=%5.1f hold=%5.1f int=%+.2f pred=%.2f fan=%d cut=%d blk=%d\n", t, pv, u, c.pHold(), c.integral(), c.predicted(), vline ? 1 : 0, 0, block ? 1 : 0);
    if (r.reach < 0 && pv >= 37.4f) r.reach = t;
    if (t < tEv && pv > r.peak) r.peak = pv;
    if (t >= tEv - 1800 && t < tEv) { double e = pv - 37.5; sqPre += e * e; ++nPre; }
    if (t < tEv && std::fabs(pv - 37.5) > 0.205) lastOut2 = t;
    if (t >= tEv) {
      r.minAfter = std::min(r.minAfter, (double)pv); r.maxAfter = std::max(r.maxAfter, (double)pv);
      r.iae += std::fabs(pv - 37.5) * 0.1 / 60.0;
      if (std::fabs(pv - 37.5) > 0.105) lastOut = t;
      if (pv >= 37.595f) { r.tOver += 0.1 / 60.0; overRun += 0.1 / 60.0; r.tOver10 = std::max(r.tOver10, overRun); } else overRun = 0;
      r.lowestHold = std::min(r.lowestHold, (double)c.pHold());
    }
  }
  r.rmsPre = std::sqrt(sqPre / std::max(1L, nPre)); r.rec = lastOut < 0 ? 0 : (lastOut - tEv) / 60.0; r.settle2 = lastOut2 < 0 ? 0 : lastOut2 / 60.0;
  return r;
}
int main(int argc, char **argv) {
  const double ventW = argc > 1 ? atof(argv[1]) : 100.0;
  std::vector<PlantParams> ov;
  for (auto p : family(11, 150, false)) { p.ventW = ventW; ov.push_back(p); }
  for (auto p : family(23, 60, true)) { p.ventW = ventW; ov.push_back(p); }
  int scenMask = argc > 2 ? atoi(argv[2]) : 1023;
  const char *dk = getenv("DUMP"); size_t only = dk ? (size_t)atoi(dk) : 0;
  for (size_t k = 0; k < ov.size(); ++k) for (int sc = 0; sc < 10; ++sc) {
    if (dk && k != only) continue;
    if (!(scenMask & (1 << sc))) continue;
    Res r = run(ov[k], sc);
    std::printf("%zu,%d,%.0f,%.2f,%.2f,%.2f,%.1f,%.1f,%.3f,%.1f,%.0f,%.1f,%.1f,%.1f,%.1f,%.0f,%.1f,%.0f\n", k, sc, r.reach, r.peak, r.minAfter, r.maxAfter, r.rec, r.iae, r.rmsPre, ov[k].UA * 9.5 / ov[k].Pmax * 100.0, ov[k].td, r.tOver, r.tOver10, r.settle2, r.fanOnMin, r.fanStarts, r.lowestHold, r.highs);
  }
}
