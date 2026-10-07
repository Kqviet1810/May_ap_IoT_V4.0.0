// Adaptive Thermal V1 simulation driver. See thermal-adaptive-sim.h for the honesty notes.
#include "thermal-adaptive-sim.h"
#include <cstring>
#include <fstream>
#include <iomanip>
using namespace sim;

static const char *HEADER =
    "label,class,mode,sp,eff,capacity,loss,dead,lag,ambient,resolution,overshoot,mae,p95,ripple,settling,rise_s,high,emergency,energy_j,target,"
    "confidence,state,kh_err_pct,delay_err_s,hold_err_pp,coast_err_c,t_qualified_s,gain,delay,hold,coast,pred_err,outliers,saves\n";

static void row(std::ostream &o, const std::string &label, const Plant &p, const Scenario &sc, const Result &r) {
  o << label << ',' << reachName(r.reach) << ',' << modeName(sc.mode) << ',' << sc.sp << ',' << p.eff << ',' << p.capacity << ','
    << p.loss << ',' << p.dead << ',' << p.lag << ',' << p.ambient << ',' << p.resolution << ',' << r.overshoot << ',' << r.mae << ','
    << r.p95 << ',' << r.ripple << ',' << r.settling << ',' << r.riseS << ',' << r.high << ',' << r.emergency << ',' << r.energyJ << ','
    << (r.pass ? "PASS" : "FAIL") << ',' << r.confidence << ',' << MayapThermal::learnStateName(static_cast<MayapThermal::LearnState>(r.state))
    << ',' << r.khEstErrPct << ',' << r.delayErrS << ',' << r.holdErrPp << ',' << r.coastErrC << ',' << r.timeQualifiedS << ','
    << r.gainEst << ',' << r.delayEst << ',' << r.holdEst << ',' << r.coastEst << ',' << r.predErr << ',' << r.outliers << ',' << r.saves << '\n';
}

// ---------------------------------------------------------------------------------------------
// Case generators
// ---------------------------------------------------------------------------------------------
struct Case { Plant p; Scenario sc; std::string label; };

// Spec matrix: heater effectiveness x thermal mass x heat loss x dead time x ambient (full
// factorial at SP 37.5) plus further setpoints on a deterministic subsample (no overfit to 37.5).
static std::vector<Case> matrixCases() {
  const double effs[] = {0.5, 0.7, 1.0, 1.2, 1.5, 2.0};
  const double caps[] = {180000, 600000, 1600000};
  const double losses[] = {120, 180, 300};
  const double deads[] = {0, 15, 30, 60, 120};
  const double ambs[] = {10, 20, 25, 28, 35};
  const float otherSp[] = {30.0f, 32.5f, 35.0f};
  std::vector<Case> out;
  unsigned k = 0;
  for (double eff : effs) for (double cap : caps) for (double loss : losses) for (double dead : deads) for (double amb : ambs) {
    Plant p; p.eff = eff; p.capacity = cap; p.loss = loss; p.dead = dead; p.ambient = amb;
    p.lag = (k % 3 == 0) ? 3 : (k % 3 == 1) ? 8 : 30;
    p.resolution = (k % 5 < 3) ? 0.1 : 0.01;
    p.bias = (k % 7 == 0) ? 0.08 : 0.0;
    for (int spi = -1; spi < 3; ++spi) {
      float sp = spi < 0 ? 37.5f : otherSp[spi];
      if (spi >= 0 && (k % 5) != static_cast<unsigned>(spi)) continue;  // subsample the other setpoints
      Case c; c.p = p; c.sc.sp = sp; c.sc.durationS = 10800; c.label = "m" + std::to_string(out.size());
      out.push_back(c);
    }
    ++k;
  }
  return out;
}

// The fixed 788-case matrix of docs/thermal-phase1-simulation.md (720 cold starts + 68 disturbances).
static std::vector<Case> legacyCases() {
  std::vector<Case> out;
  const double capacities[] = {180000, 600000, 1600000};
  const unsigned delays[] = {0, 5, 15, 30, 60, 120};
  const double ambients[] = {10, 20, 25, 28, 35};
  for (double sp : {30., 32., 35., 37.5}) for (unsigned ai = 0; ai < 5; ++ai) for (unsigned ci = 0; ci < 3; ++ci)
    for (unsigned di = 0; di < 6; ++di) for (unsigned ri = 0; ri < 2; ++ri) {
      const unsigned k = ai * 36 + ci * 12 + di * 2 + ri;
      Case c; c.p.ambient = ambients[ai]; c.p.capacity = capacities[ci]; c.p.loss = ci == 0 ? 120. : ci == 1 ? 180. : 300.;
      c.p.lag = k % 3 == 0 ? 3. : k % 3 == 1 ? 8. : 30.; c.p.eff = k % 3 == 0 ? .7 : k % 3 == 1 ? 1. : 1.2;
      c.p.resolution = ri == 0 ? .1 : .01; c.p.bias = k % 7 == 0 ? .08 : 0.; c.p.dead = delays[di];
      c.sc.sp = static_cast<float>(sp); c.sc.durationS = 10800; c.sc.event = "none"; c.label = "cold";
      out.push_back(c);
    }
  const char *events[] = {"door1", "door2", "door5", "ambient_rise", "loss_up", "loss_down", "load_heavy", "load_light", "vent",
                          "sensor_loss", "bad_sample", "sensor_frozen", "safety_cut", "power_recovery", "setpoint_step", "noise", "jitter_drop"};
  for (const char *ev : events) for (unsigned di : {0U, 15U, 60U, 120U}) {
    Case c; c.p.ambient = 25; c.p.capacity = di % 2 ? 600000. : 180000.; c.p.loss = di % 2 ? 180. : 120.; c.p.lag = di % 2 ? 8. : 30.;
    c.p.eff = 1.0; c.p.resolution = .01; c.p.bias = 0; c.p.dead = di; c.sc.sp = 37.5f; c.sc.durationS = 10800; c.label = ev;
    const std::string e = ev;
    c.sc.eventAtS = 3600;
    if (e == "door1") { c.sc.event = "door"; c.sc.eventValue = 1; } else if (e == "door2") { c.sc.event = "door"; c.sc.eventValue = 2; }
    else if (e == "door5") { c.sc.event = "door"; c.sc.eventValue = 5; }
    else if (e == "ambient_rise") { c.sc.changeAtS = 3600; c.sc.ambientDelta = 3; }
    else if (e == "loss_up") { c.sc.changeAtS = 3600; c.sc.lossScale = 1.6; } else if (e == "loss_down") { c.sc.changeAtS = 3600; c.sc.lossScale = 0.6; }
    else if (e == "load_heavy") { c.sc.changeAtS = 3600; c.sc.capScale = 2; } else if (e == "load_light") { c.sc.changeAtS = 3600; c.sc.capScale = 0.5; }
    else if (e == "vent") { c.sc.changeAtS = 3600; c.sc.extraLossW = 200; c.sc.extraLossUntilS = 4200; }
    else if (e == "sensor_loss") c.sc.event = "sensor_loss"; else if (e == "bad_sample") c.sc.event = "sensor_invalid";
    else if (e == "sensor_frozen") c.sc.event = "sensor_frozen"; else if (e == "safety_cut") c.sc.event = "safety_cut";
    else if (e == "power_recovery") c.sc.event = "power_recovery";
    else if (e == "setpoint_step") { c.sc.event = "setpoint_step"; c.sc.spBefore = 30.0f; }
    else if (e == "noise") c.sc.event = "noisy"; else if (e == "jitter_drop") c.sc.event = "jitter";
    out.push_back(c);
  }
  return out;
}

static int shardRun(const std::vector<Case> &cases, int shard, int shards, std::ostream &o, const std::vector<Mode> &modes) {
  for (size_t i = 0; i < cases.size(); ++i) {
    if (static_cast<int>(i % shards) != shard) continue;
    for (Mode m : modes) {
      Scenario sc = cases[i].sc; sc.mode = m; sc.label = cases[i].label;
      const Result r = run(cases[i].p, sc);
      row(o, cases[i].label + "#" + std::to_string(i), cases[i].p, sc, r);
    }
  }
  return 0;
}

int main(int argc, char **argv) {
  std::string cmd = argc > 1 ? argv[1] : "one";
  std::cout << std::fixed << std::setprecision(4);
  if (cmd == "one") {
    Plant p; Scenario sc; bool csv = false;
    for (int i = 2; i + 1 < argc; i += 2) {
      const std::string k = argv[i]; const double v = std::atof(argv[i + 1]);
      if (k == "--eff") p.eff = v; else if (k == "--cap") p.capacity = v; else if (k == "--loss") p.loss = v;
      else if (k == "--dead") p.dead = v; else if (k == "--lag") p.lag = v; else if (k == "--amb") p.ambient = v;
      else if (k == "--ventG") p.ventG = v; else if (k == "--res") p.resolution = v; else if (k == "--sp") sc.sp = static_cast<float>(v);
      else if (k == "--mode") sc.mode = static_cast<Mode>(static_cast<int>(v)); else if (k == "--hours") sc.durationS = v * 3600;
      else if (k == "--vcycle") { sc.ventProfile = true; sc.ventCycleMin = static_cast<unsigned>(v); }
      else if (k == "--vduty") { sc.ventProfile = true; sc.ventDutyPct = static_cast<unsigned>(v); }
      else if (k == "--voff") sc.ventOffsetSec = static_cast<uint32_t>(v);
      else if (k == "--changeAt") sc.changeAtS = v * 3600; else if (k == "--effScale") sc.effScale = v;
      else if (k == "--ventScale") sc.ventScale = v; else if (k == "--lossScale") sc.lossScale = v; else if (k == "--capScale") sc.capScale = v;
      else if (k == "--debugFrom") { sc.debugOut = &std::cerr; sc.debugFrom = v; sc.debugTo = v + 600; }
      else if (k == "--trace") sc.trace = v != 0; else if (k == "--csv") csv = v != 0;
    }
    sc.label = "one";
    if (sc.trace) { sc.traceOut = &std::cerr;
      std::cerr << "label,mode,t,temp,fed,sp,duty,vent,conf,state,gain,delay,hold,coast,ventGain,predErr,mismatch,ventPhase,ff,integral,gate,req\n"; }
    const Result r = run(p, sc);
    if (csv) { std::cout << HEADER; row(std::cout, "one", p, sc, r); }
    else {
      std::cout << reachName(r.reach) << " mode=" << modeName(sc.mode) << " overshoot=" << r.overshoot << " mae=" << r.mae << " p95=" << r.p95
                << " ripple=" << r.ripple << " settle=" << r.settling << " rise=" << r.riseS << " high=" << r.high << " emerg=" << r.emergency
                << " pass=" << r.pass << "\n";
      if (sc.mode != Mode::Baseline)
        std::cout << "conf=" << r.confidence << " state=" << MayapThermal::learnStateName(static_cast<MayapThermal::LearnState>(r.state))
                  << " Kh true=" << p.khTrue() << " est=" << r.gainEst << " (" << r.khEstErrPct << "%) delay true=" << p.dead + p.lag
                  << " est=" << r.delayEst << " hold true=" << holdTruthPct(p, sc.sp) << " est=" << r.holdEst << " coast true=" << coastTruthC(p)
                  << " est=" << r.coastEst << " vent true=" << ventTruth(p, sc.sp) << " est=" << r.ventEst << " conf=" << r.ventConf
                  << " predErr=" << r.predErr << " tQual=" << r.timeQualifiedS << "\n";
      if (r.ventEvents) std::cout << "vents=" << r.ventEvents << " devMax=" << r.ventDevMax << " postMax=" << r.postVentOvershootMax
                                  << " recMax=" << r.ventRecoveryMax << " windupMax=" << r.ventWindupMax << " energyMean=" << r.ventEnergyMean << "\n";
    }
    return 0;
  }
  if (cmd == "matrix" || cmd == "legacy788") {
    const int shard = argc > 2 ? std::atoi(argv[2]) : 0, shards = argc > 3 ? std::atoi(argv[3]) : 1;
    std::ostream *o = &std::cout; std::ofstream f;
    if (argc > 4) { f.open(argv[4]); o = &f; }
    *o << std::fixed << std::setprecision(4);
    if (shard == 0) *o << HEADER;
    return shardRun(cmd == "matrix" ? matrixCases() : legacyCases(), shard, shards, *o, {Mode::Baseline, Mode::Adaptive});
  }
  if (cmd == "count") { std::cout << "matrix=" << matrixCases().size() << " legacy788=" << legacyCases().size() << "\n"; return 0; }
  if (cmd == "mini") {  // quick development matrix: 54 representative plants, BASELINE vs ADAPTIVE
    int pass[2] = {0, 0}, n = 0, high[2] = {0, 0}, em[2] = {0, 0};
    double maeSum[2] = {0, 0}, maeMax[2] = {0, 0}, ovMax[2] = {-9, -9};
    const int hdr = argc > 2 ? std::atoi(argv[2]) : 1;
    if (hdr) std::cout << "eff cap loss dead | BASE: ov mae p95 rip settle | V1: ov mae p95 rip settle conf khErr% dErr hErr cErr\n";
    for (double eff : {0.5, 1.0, 2.0}) for (double cap : {180000., 600000., 1600000.}) for (double loss : {120., 300.})
      for (double dead : {0., 30., 120.}) {
        Plant p; p.eff = eff; p.capacity = cap; p.loss = loss; p.dead = dead; p.lag = dead == 0 ? 3 : 8; p.ambient = 20; p.resolution = 0.1;
        if (classify(p, 37.5) != Reach::Reachable) continue;
        Result r[2];
        for (int m = 0; m < 2; ++m) {
          Scenario sc; sc.mode = m ? Mode::Adaptive : Mode::Baseline; sc.label = "mini"; r[m] = run(p, sc);
          pass[m] += r[m].pass; high[m] += r[m].high; em[m] += r[m].emergency; maeSum[m] += r[m].mae;
          maeMax[m] = std::max(maeMax[m], r[m].mae); ovMax[m] = std::max(ovMax[m], r[m].overshoot);
        }
        ++n;
        if (hdr > 1) std::cout << eff << ' ' << cap / 1000 << ' ' << loss << ' ' << dead << " | " << r[0].overshoot << ' ' << r[0].mae << ' ' << r[0].p95 << ' '
          << r[0].ripple << ' ' << r[0].settling << " | " << r[1].overshoot << ' ' << r[1].mae << ' ' << r[1].p95 << ' ' << r[1].ripple << ' '
          << r[1].settling << ' ' << r[1].confidence << ' ' << r[1].khEstErrPct << ' ' << r[1].delayErrS << ' ' << r[1].holdErrPp << ' ' << r[1].coastErrC << '\n';
      }
    std::cout << "reachable plants=" << n << "\n";
    for (int m = 0; m < 2; ++m)
      std::cout << (m ? "ADAPTIVE" : "BASELINE") << ": pass=" << pass[m] << " High=" << high[m] << " Emerg=" << em[m] << " maeMean=" << maeSum[m] / n
                << " maeMax=" << maeMax[m] << " overshootMax=" << ovMax[m] << "\n";
    return 0;
  }
  return 2;
}
