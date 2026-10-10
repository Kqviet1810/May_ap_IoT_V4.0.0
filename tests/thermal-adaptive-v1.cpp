// Adaptive Thermal V1 simulation driver. See thermal-adaptive-sim.h for the honesty notes.
#include "thermal-adaptive-sim.h"
#include <cstring>
#include <fstream>
#include <iomanip>
using namespace sim;

static const char *HEADER =
    "label,class,mode,sp,eff,capacity,loss,dead,lag,ambient,resolution,overshoot,mae,p95,ripple,settling,rise_s,high,emergency,energy_j,target,"
    "confidence,state,kh_err_pct,delay_err_s,hold_err_pp,coast_err_c,t_qualified_s,gain,delay,hold,coast,pred_err,outliers,saves,mismatch\n";

static void row(std::ostream &o, const std::string &label, const Plant &p, const Scenario &sc, const Result &r) {
  o << label << ',' << reachName(r.reach) << ',' << modeName(sc.mode) << ',' << sc.sp << ',' << p.eff << ',' << p.capacity << ','
    << p.loss << ',' << p.dead << ',' << p.lag << ',' << p.ambient << ',' << p.resolution << ',' << r.overshoot << ',' << r.mae << ','
    << r.p95 << ',' << r.ripple << ',' << r.settling << ',' << r.riseS << ',' << r.high << ',' << r.emergency << ',' << r.energyJ << ','
    << (r.pass ? "PASS" : "FAIL") << ',' << r.confidence << ',' << MayapThermal::learnStateName(static_cast<MayapThermal::LearnState>(r.state))
    << ',' << r.khEstErrPct << ',' << r.delayErrS << ',' << r.holdErrPp << ',' << r.coastErrC << ',' << r.timeQualifiedS << ','
    << r.gainEst << ',' << r.delayEst << ',' << r.holdEst << ',' << r.coastEst << ',' << r.predErr << ',' << r.outliers << ',' << r.saves << ',' << r.mismatchEvents << '\n';
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

// HOLDOUT matrix (Smart Thermal): parameter values that appear in NEITHER the 788 nor the 2160 matrix, so an improvement
// that only fits those grids shows up as a gap here. Fixed, deterministic, never edited after the first A/B run.
static std::vector<Case> holdoutCases() {
  const double effs[] = {0.6, 0.85, 1.1, 1.35, 1.75};
  const double caps[] = {250000, 900000, 1300000};
  const double losses[] = {150, 240};
  const double deads[] = {8, 25, 45, 90};
  const double ambs[] = {12, 22, 31};
  std::vector<Case> out;
  unsigned k = 0;
  for (double eff : effs) for (double cap : caps) for (double loss : losses) for (double dead : deads) for (double amb : ambs) {
    Plant p; p.eff = eff; p.capacity = cap; p.loss = loss; p.dead = dead; p.ambient = amb;
    const double lags[] = {5, 15, 20};
    p.lag = lags[k % 3];
    p.resolution = (k % 4 == 0) ? 0.05 : (k % 4 == 1) ? 0.1 : (k % 4 == 2) ? 0.02 : 0.1;
    p.bias = (k % 6 == 0) ? 0.05 : 0.0;
    p.noise = (k % 5 == 0) ? 0.02 : 0.0;
    Case c; c.p = p; c.sc.sp = (k % 3 == 0) ? 36.5f : 37.5f; c.sc.durationS = 10800; c.label = "h" + std::to_string(out.size());
    out.push_back(c);
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

// ---------------------------------------------------------------------------------------------
// Ventilation scenarios (the exhaust fan is owned by the real profile logic; thermal only coordinates)
// ---------------------------------------------------------------------------------------------
struct VentCase { Plant p; Scenario sc; std::string plant, strength, schedule; };
static std::vector<VentCase> ventCases(bool extended = false) {
  struct PL { const char *n; double cap, loss, dead, lag; };
  const PL plants[] = {{"light", 180000, 120, 5, 8}, {"medium", 600000, 180, 15, 8}, {"heavy", 1600000, 300, 30, 8}};
  struct EX { const char *n; double g; };
  const EX exhaustStd[] = {{"weak", 40}, {"medium", 120}, {"strong", 300}};
  const EX exhaustX[] = {{"very_strong_600", 600}};
  const EX *exhaust = extended ? exhaustX : exhaustStd;
  const size_t nExhaust = extended ? 1 : 3;
  struct SC { const char *n; unsigned cycle, duty; double firstAtS; double changeAtS; double ventScale; };
  const SC scheds[] = {{"short_3min", 20, 15, 4000, -1, 1}, {"long_12min", 40, 30, 4000, -1, 1}, {"repeated_3min_12cycle", 12, 25, 4000, -1, 1},
                       {"during_heatup", 40, 10, 400, -1, 1}, {"near_sp", 40, 10, 2400, -1, 1}, {"after_stable_hold", 40, 10, 6000, -1, 1},
                       {"fan_effect_x2_midrun", 20, 15, 4000, 10800, 2.0}};
  std::vector<VentCase> out;
  for (const PL &pl : plants) for (size_t xi = 0; xi < nExhaust; ++xi) for (const SC &sch : scheds) {
    const EX &ex = exhaust[xi];
    VentCase c; c.p.eff = 1.0; c.p.capacity = pl.cap; c.p.loss = pl.loss; c.p.dead = pl.dead; c.p.lag = pl.lag; c.p.ambient = 20;
    c.p.resolution = 0.1; c.p.ventG = ex.g;
    c.sc.sp = 37.5f; c.sc.durationS = 21600; c.sc.ventProfile = true; c.sc.ventCycleMin = sch.cycle; c.sc.ventDutyPct = sch.duty;
    const unsigned cycleSec = sch.cycle * 60;
    c.sc.ventOffsetSec = cycleSec - (static_cast<unsigned>(sch.firstAtS) % cycleSec);
    if (c.sc.ventOffsetSec >= cycleSec) c.sc.ventOffsetSec -= cycleSec;
    c.sc.changeAtS = sch.changeAtS; c.sc.ventScale = sch.ventScale;
    c.sc.metricsFromS = (std::string(sch.n) == "during_heatup" || std::string(sch.n) == "near_sp") ? 0.0 : 3600.0;
    c.plant = pl.n; c.strength = ex.n; c.schedule = sch.n;
    out.push_back(c);
  }
  if (extended) {   // environment HOTTER than the set point: the exhaust fan brings heat in (cooling is not available)
    const PL hotPlants[] = {{"medium", 600000, 180, 15, 8}, {"heavy", 1600000, 300, 30, 8}};
    const EX hotEx[] = {{"hot_ambient_120", 120}, {"hot_ambient_300", 300}};
    for (const PL &pl : hotPlants) for (const EX &ex : hotEx) {
      VentCase c; c.p.eff = 1.0; c.p.capacity = pl.cap; c.p.loss = pl.loss; c.p.dead = pl.dead; c.p.lag = pl.lag; c.p.ambient = 40;
      c.p.resolution = 0.1; c.p.ventG = ex.g;
      c.sc.sp = 37.5f; c.sc.durationS = 21600; c.sc.ventProfile = true; c.sc.ventCycleMin = 40; c.sc.ventDutyPct = 10;
      const unsigned cycleSec = 40 * 60;
      c.sc.ventOffsetSec = cycleSec - (6000u % cycleSec);
      if (c.sc.ventOffsetSec >= cycleSec) c.sc.ventOffsetSec -= cycleSec;
      c.sc.metricsFromS = 3600.0;
      c.plant = pl.n; c.strength = ex.n; c.schedule = "after_stable_hold";
      out.push_back(c);
    }
  }
  return out;
}
static const char *VHEADER =
    "plant,exhaust,schedule,mode,vent_events,drop_events,vent_dev_max,vent_dev_mean,post_vent_overshoot_max,post_vent_overshoot_mean,recovery_s_max,"
    "heater_on_s_mean,integral_windup_max,high,emergency,mae_tail,ripple,confidence,vent_confidence,vent_gain_est,vent_gain_true,vent_err_pct,kh_err_pct\n";

// ---------------------------------------------------------------------------------------------
// Hardware change
// ---------------------------------------------------------------------------------------------
struct HwCase { Plant p; Scenario sc; std::string label, change; };
static std::vector<HwCase> hwCases() {
  std::vector<HwCase> out;
  struct PL { const char *n; double cap, loss, dead, lag; };
  const PL plants[] = {{"light_d15", 180000, 120, 15, 8}, {"medium_d15", 600000, 180, 15, 8}, {"medium_d60", 600000, 180, 60, 8}, {"heavy_d30", 1600000, 300, 30, 8}};
  struct CH { const char *n; double eff, vent; };
  const CH changes[] = {{"heater_100_to_150", 1.5, 1.0}, {"heater_100_to_60", 0.6, 1.0}, {"vent_1_to_2", 1.0, 2.0}, {"no_change_control", 1.0, 1.0}};
  for (const PL &pl : plants) for (const CH &ch : changes) {
    HwCase c; c.p.eff = 1.0; c.p.capacity = pl.cap; c.p.loss = pl.loss; c.p.dead = pl.dead; c.p.lag = pl.lag; c.p.ambient = 20; c.p.resolution = 0.1;
    c.p.ventG = 120;
    c.sc.sp = 37.5f; c.sc.durationS = 25200; c.sc.changeAtS = 10800; c.sc.effScale = ch.eff; c.sc.ventScale = ch.vent;
    c.sc.ventProfile = true; c.sc.ventCycleMin = 20; c.sc.ventDutyPct = 15; c.sc.ventOffsetSec = 20 * 60 - 700;
    c.label = pl.n; c.change = ch.n; out.push_back(c);
  }
  return out;
}
static const char *HHEADER =
    "plant,change,mode,high,emergency,mae_tail,p95_tail,ripple_tail,overshoot,conf_final,min_conf_after,t_qualified_s,t_mismatch_s,t_reconverge_s,"
    "min_hint_after,ff_min_after,ff_max_after,kh_err_pct,delay_err_s,vent_err_pct,mismatch_final,guard_events\n";

// ---------------------------------------------------------------------------------------------
// Sensor / disturbance faults and learning integrity
// ---------------------------------------------------------------------------------------------
struct FaultCase { Plant p; Scenario sc; std::string label; };
static std::vector<FaultCase> faultCases() {
  std::vector<FaultCase> out;
  struct PL { const char *n; double cap, loss, dead, lag, res; };
  const PL plants[] = {{"medium_d15", 600000, 180, 15, 8, 0.1}, {"heavy_d60", 1600000, 300, 60, 8, 0.1}};
  const char *events[] = {"sensor_loss", "sensor_invalid", "sudden_jump", "sensor_frozen", "noisy", "jitter", "missing_samples", "reboot",
                          "manual_test", "heater_off", "safety_cut", "power_loss", "door", "rollover", "vent_transition"};
  for (const PL &pl : plants) for (const char *ev : events) {
    FaultCase c; c.p.capacity = pl.cap; c.p.loss = pl.loss; c.p.dead = pl.dead; c.p.lag = pl.lag; c.p.resolution = pl.res; c.p.ambient = 20; c.p.ventG = 120;
    c.sc.sp = 37.5f; c.sc.durationS = 14400; c.sc.eventAtS = 7200; c.sc.event = ev; c.label = std::string(pl.n) + "/" + ev;
    if (std::string(ev) == "rollover") { c.sc.event = "none"; c.sc.clockOffset = 0xFFFFFFFFU - 1000U - 7200000U + 5U; }
    if (std::string(ev) == "vent_transition") { c.sc.event = "none"; c.sc.ventProfile = true; c.sc.ventCycleMin = 20; c.sc.ventDutyPct = 15; c.sc.ventOffsetSec = 20 * 60 - 7000; }
    if (std::string(ev) == "door") c.sc.eventValue = 2.0;
    out.push_back(c);
  }
  return out;
}
static const char *FHEADER =
    "case,mode,high,emergency,overshoot,mae_tail,ripple_tail,conf_final,state,fault_updates,conf_after_reboot,guard_events,gain_err_pct\n";

int main(int argc, char **argv) {
  std::string cmd = argc > 1 ? argv[1] : "one";
  // "<suite>_smart" runs the same case generator with the Smart Thermal mode only (A/B joins on the case label).
  bool smartOnly = false;
  if (cmd.size() > 6 && cmd.compare(cmd.size() - 6, 6, "_smart") == 0) { smartOnly = true; cmd.resize(cmd.size() - 6); }
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
      std::cerr << "label,mode,t,temp,fed,sp,duty,vent,conf,state,gain,delay,hold,coast,ventGain,predErr,mismatch,ventPhase,ff,integral,gate,req,holdWin,info\n"; }
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
      std::cout << "mismatch events=" << r.mismatchEvents << " (by Kh-ratio=" << r.mismatchKh << ", by hold=" << r.mismatchHold << ")\n";
      if (r.ventEvents) std::cout << "vents=" << r.ventEvents << " devMax=" << r.ventDevMax << " postMax=" << r.postVentOvershootMax
                                  << " recMax=" << r.ventRecoveryMax << " windupMax=" << r.ventWindupMax << " energyMean=" << r.ventEnergyMean << "\n";
    }
    return 0;
  }
  if (cmd == "matrix" || cmd == "legacy788" || cmd == "holdout") {
    const int shard = argc > 2 ? std::atoi(argv[2]) : 0, shards = argc > 3 ? std::atoi(argv[3]) : 1;
    std::ostream *o = &std::cout; std::ofstream f;
    if (argc > 4) { f.open(argv[4]); o = &f; }
    *o << std::fixed << std::setprecision(4);
    if (shard == 0) *o << HEADER;
    return shardRun(cmd == "matrix" ? matrixCases() : cmd == "holdout" ? holdoutCases() : legacyCases(), shard, shards, *o, smartOnly ? std::vector<Mode>{Mode::Smart} : std::vector<Mode>{Mode::Baseline, Mode::Adaptive});
  }
  if (cmd == "vent" || cmd == "ventx") {
    const int shard = argc > 2 ? std::atoi(argv[2]) : 0, shards = argc > 3 ? std::atoi(argv[3]) : 1;
    std::ostream *o = &std::cout; std::ofstream f;
    if (argc > 4) { f.open(argv[4]); o = &f; }
    *o << std::fixed << std::setprecision(4);
    if (shard == 0) *o << VHEADER;
    const auto cases = ventCases(cmd == "ventx");
    for (size_t i = 0; i < cases.size(); ++i) {
      if (static_cast<int>(i % shards) != shard) continue;
      for (Mode m : (smartOnly ? std::vector<Mode>{Mode::Smart} : std::vector<Mode>{Mode::Baseline, Mode::LearnNoVent, Mode::Adaptive})) {
        Scenario sc = cases[i].sc; sc.mode = m; sc.label = cases[i].plant + "/" + cases[i].strength + "/" + cases[i].schedule;
        const Result r = run(cases[i].p, sc);
        *o << cases[i].plant << ',' << cases[i].strength << ',' << cases[i].schedule << ',' << modeName(m) << ',' << r.ventEvents << ',' << r.ventDropEvents << ',' << r.ventDevMax << ','
           << r.ventDevMean << ',' << r.postVentOvershootMax << ',' << r.postVentOvershootMean << ',' << r.ventRecoveryMax << ',' << r.ventEnergyMean << ','
           << r.ventWindupMax << ',' << r.high << ',' << r.emergency << ',' << r.mae << ',' << r.ripple << ',' << r.confidence << ',' << r.ventConf << ','
           << r.ventEst << ',' << ventTruth(cases[i].p, 37.5) << ',' << r.ventErrPct << ',' << r.khEstErrPct << '\n';
      }
    }
    return 0;
  }
  if (cmd == "hwchange") {
    const int shard = argc > 2 ? std::atoi(argv[2]) : 0, shards = argc > 3 ? std::atoi(argv[3]) : 1;
    std::ostream *o = &std::cout; std::ofstream f;
    if (argc > 4) { f.open(argv[4]); o = &f; }
    *o << std::fixed << std::setprecision(4);
    if (shard == 0) *o << HHEADER;
    const auto cases = hwCases();
    for (size_t i = 0; i < cases.size(); ++i) {
      if (static_cast<int>(i % shards) != shard) continue;
      for (Mode m : (smartOnly ? std::vector<Mode>{Mode::Smart} : std::vector<Mode>{Mode::Baseline, Mode::Adaptive})) {
        Scenario sc = cases[i].sc; sc.mode = m; sc.label = cases[i].label + "/" + cases[i].change;
        const Result r = run(cases[i].p, sc);
        *o << cases[i].label << ',' << cases[i].change << ',' << modeName(m) << ',' << r.high << ',' << r.emergency << ',' << r.mae << ',' << r.p95 << ',' << r.ripple << ','
           << r.overshoot << ',' << r.confidence << ',' << r.minConfAfterChange << ',' << r.timeQualifiedS << ',' << r.timeMismatchS << ',' << r.timeReconvergeS << ','
           << r.minHintAfterChange << ',' << r.minFfAfterChange << ',' << r.maxFfAfterChange << ',' << r.khEstErrPct << ',' << r.delayErrS << ',' << r.ventErrPct << ','
           << r.mismatchEvents << ',' << r.overshootGuards << '\n';
      }
    }
    return 0;
  }
  if (cmd == "faults") {
    const int shard = argc > 2 ? std::atoi(argv[2]) : 0, shards = argc > 3 ? std::atoi(argv[3]) : 1;
    std::ostream *o = &std::cout; std::ofstream f;
    if (argc > 4) { f.open(argv[4]); o = &f; }
    *o << std::fixed << std::setprecision(4);
    if (shard == 0) *o << FHEADER;
    const auto cases = faultCases();
    for (size_t i = 0; i < cases.size(); ++i) {
      if (static_cast<int>(i % shards) != shard) continue;
      for (Mode m : (smartOnly ? std::vector<Mode>{Mode::Smart} : std::vector<Mode>{Mode::Baseline, Mode::Adaptive})) {
        Scenario sc = cases[i].sc; sc.mode = m; sc.label = cases[i].label;
        const Result r = run(cases[i].p, sc);
        *o << cases[i].label << ',' << modeName(m) << ',' << r.high << ',' << r.emergency << ',' << r.overshoot << ',' << r.mae << ',' << r.ripple << ',' << r.confidence << ','
           << MayapThermal::learnStateName(static_cast<MayapThermal::LearnState>(r.state)) << ',' << r.faultUpdates << ',' << r.confAfterReboot << ',' << r.overshootGuards << ','
           << r.khEstErrPct << '\n';
      }
    }
    return 0;
  }
  if (cmd == "case") {   // case <matrix|legacy788> <label#idx> <mode 0..3> [debugFrom]: one named case of a matrix, optionally with per-sample internals
    const std::string suite = argc > 2 ? argv[2] : "", want = argc > 3 ? argv[3] : "";
    if (suite == "hwchange") {   // label = "<plant>/<change>"
      for (const HwCase &h : hwCases()) {
        if (h.label + "/" + h.change != want) continue;
        Scenario sc = h.sc; sc.mode = static_cast<Mode>(argc > 4 ? std::atoi(argv[4]) : 2); sc.label = want;
        if (argc > 5 && std::atof(argv[5]) < 0) { sc.trace = true; sc.traceOut = &std::cerr;
          std::cerr << "label,mode,t,temp,fed,sp,duty,vent,conf,state,gain,delay,hold,coast,ventGain,predErr,mismatch,ventPhase,ff,integral,gate,req,holdWin,info\n"; }
        if (argc > 5 && std::atof(argv[5]) >= 0) { sc.debugOut = &std::cerr; sc.debugFrom = std::atof(argv[5]); sc.debugTo = sc.debugFrom + 600; }
        const Result r = run(h.p, sc);
        std::cout << want << " " << modeName(sc.mode) << " ov=" << r.overshoot << " mae=" << r.mae << " conf=" << r.confidence << " mism=" << r.mismatchEvents
                  << " byKh=" << r.mismatchKh << " byHold=" << r.mismatchHold << " tMismatch=" << r.timeMismatchS << "\n";
        return 0;
      }
      return 2;
    }
    const std::vector<Case> all = suite == "matrix" ? matrixCases() : suite == "holdout" ? holdoutCases() : legacyCases();
    for (size_t i = 0; i < all.size(); ++i) {
      if (all[i].label + "#" + std::to_string(i) != want) continue;
      Scenario sc = all[i].sc; sc.mode = static_cast<Mode>(argc > 4 ? std::atoi(argv[4]) : 2); sc.label = all[i].label;
      if (argc > 5 && std::atof(argv[5]) >= 0) { sc.debugOut = &std::cerr; sc.debugFrom = std::atof(argv[5]); sc.debugTo = sc.debugFrom + 600; }
      if (argc > 5 && std::atof(argv[5]) < 0) { sc.trace = true; sc.traceOut = &std::cerr;   // trace every 60 s (learner state, gate, mismatch)
        std::cerr << "label,mode,t,temp,fed,sp,duty,vent,conf,state,gain,delay,hold,coast,ventGain,predErr,mismatch,ventPhase,ff,integral,gate,req,holdWin,info\n"; }
      const Result r = run(all[i].p, sc);
      std::cout << want << " " << modeName(sc.mode) << " ov=" << r.overshoot << " mae=" << r.mae << " p95=" << r.p95 << " ripple=" << r.ripple
                << " settle=" << r.settling << " high=" << r.high << " emerg=" << r.emergency << " pass=" << r.pass << " conf=" << r.confidence << " tQual=" << r.timeQualifiedS << " rise=" << r.riseS << " mism=" << r.mismatchEvents << " byKh=" << r.mismatchKh << " byHold=" << r.mismatchHold << "\n";
      return 0;
    }
    return 2;
  }
  if (cmd == "longrun") {   // 12 h steady and 72 h with parameter drift, vent profile and sensor noise; per-6h-block |PV-SP|
    const int shard = argc > 2 ? std::atoi(argv[2]) : 0, shards = argc > 3 ? std::atoi(argv[3]) : 1;
    std::ostream *o = &std::cout; std::ofstream f;
    if (argc > 4) { f.open(argv[4]); o = &f; }
    *o << std::fixed << std::setprecision(4);
    if (shard == 0) *o << "plant,run,mode,hours,high,emergency,overshoot,mismatch_events,profile_saves,confidence,state,block_mae_6h_0..11,block_max_6h_0..11\n";
    struct PL { const char *n; double cap, loss, dead; };
    const PL plants[] = {{"light_d15", 180000, 120, 15}, {"medium_d15", 600000, 180, 15}, {"medium_d60", 600000, 180, 60}, {"heavy_d30", 1600000, 300, 30}};
    struct RUN { const char *n; double hours; bool drift; };
    const RUN runs[] = {{"steady_12h", 12, false}, {"drift_72h", 72, true}};
    size_t idx = 0;
    for (const PL &pl : plants) for (const RUN &rn : runs) {
      if (static_cast<int>(idx++ % shards) != shard) continue;
      for (Mode m : (smartOnly ? std::vector<Mode>{Mode::Smart} : std::vector<Mode>{Mode::Baseline, Mode::Adaptive})) {
        Plant p; p.eff = 1.0; p.capacity = pl.cap; p.loss = pl.loss; p.dead = pl.dead; p.lag = 8; p.ambient = 20; p.resolution = 0.1; p.ventG = 120; p.noise = 0.03;
        Scenario sc; sc.sp = 37.5f; sc.durationS = rn.hours * 3600; sc.mode = m; sc.ventProfile = true; sc.ventCycleMin = 40; sc.ventDutyPct = 10;
        sc.ventOffsetSec = 0; sc.metricsFromS = 7200;
        if (rn.drift) { sc.changeAtS = 36 * 3600; sc.effScale = 0.9; sc.lossScale = 1.15; }   // heater ages 10 %, heat loss +15 % at 36 h
        sc.label = std::string(pl.n) + "/" + rn.n;
        const Result r = run(p, sc);
        *o << pl.n << ',' << rn.n << ',' << modeName(m) << ',' << rn.hours << ',' << r.high << ',' << r.emergency << ',' << r.overshoot << ',' << r.mismatchEvents << ','
           << r.saves << ',' << r.confidence << ',' << MayapThermal::learnStateName(static_cast<MayapThermal::LearnState>(r.state));
        for (size_t b = 0; b < 12; ++b) *o << ',' << (b < r.blockMae.size() ? r.blockMae[b] : -1.0);
        for (size_t b = 0; b < 12; ++b) *o << ',' << (b < r.blockMax.size() ? r.blockMax[b] : -1.0);
        *o << '\n';
      }
    }
    return 0;
  }
  if (cmd == "count") { std::cout << "holdout=" << holdoutCases().size() << " matrix=" << matrixCases().size() << " legacy788=" << legacyCases().size() << " vent=" << ventCases().size()
    << " hwchange=" << hwCases().size() << " faults=" << faultCases().size() << "\n"; return 0; }
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
