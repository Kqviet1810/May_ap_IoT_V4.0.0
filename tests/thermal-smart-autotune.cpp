// Smart AutoTune V1 closed-loop qualification driver (SOFTWARE / SIMULATION ONLY).
//
// The controller under test is the production AutoTune route (startAutoTune/updateAutoTune/updateHeatingAndOutputs sliced
// from machine_control.h) with the real engine, real OutputArbiter, real heater scheduler, real config store and the real
// ThermalProfile seeding. The plant only closes the loop; the firmware never sees plant parameters.
//
// Build twice from this file: default = Smart AutoTune, -DLEGACY_ENGINE = the original relay-only tune (same plants).
//
//   thermal-smart-autotune mini|full out.csv [shard count]
#include "thermal-smart-tune-sim.h"
#include <fstream>
#include <iomanip>

using namespace sim;
using namespace smarttune;

namespace {

static const char *classOf(const TuneOut &o) {
  if (!o.started) return "NOT_STARTED";
  if (o.accepted) return "ACCEPTED";
  switch (o.reason) {
    case AutoTuneReason::SafetyAbort: return "SAFETY_ABORT";
    case AutoTuneReason::SensorAbort: return "SENSOR_ABORT";
    case AutoTuneReason::ModeAbort: return "MODE_ABORT";
    case AutoTuneReason::PreheatTimeout: case AutoTuneReason::PhaseTimeout: case AutoTuneReason::TotalTimeout:
    case AutoTuneReason::ApproachTimeout: return "TIMEOUT";
    default: return "REJECTED";
  }
}

static std::vector<Case> miniCases() {
  const double effs[] = {0.5, 0.7, 1.0, 1.5, 2.0};
  const double caps[] = {180000, 600000, 1600000};
  const double deads[] = {0, 15, 30, 60, 120};
  const double losses[] = {120, 180, 300};
  const double ambs[] = {20, 28};
  const double ress[] = {0.01, 0.1};
  std::vector<Case> out;
  for (unsigned k = 0; k < 36; ++k) {
    Case c;
    c.p.eff = effs[k % 5]; c.p.capacity = caps[k % 3]; c.p.dead = deads[(2 * k + k / 5) % 5];
    c.p.loss = losses[(k / 3) % 3]; c.p.ambient = ambs[(k / 2) % 2]; c.p.resolution = ress[k % 2];
    c.p.lag = 8; c.label = "mini" + std::to_string(k);
    out.push_back(c);
  }
  return out;
}

static std::vector<Case> fullCases() {
  const double effs[] = {0.5, 0.7, 1.0, 1.2, 1.5, 2.0};
  const double caps[] = {180000, 600000, 1600000};
  const double losses[] = {120, 180, 300};
  const double deads[] = {0, 15, 30, 60, 120};
  const double ambs[] = {20, 28};
  std::vector<Case> out;
  unsigned k = 0;
  for (double eff : effs) for (double cap : caps) for (double loss : losses) for (double dead : deads) for (double amb : ambs) {
    Case c; c.p.eff = eff; c.p.capacity = cap; c.p.loss = loss; c.p.dead = dead; c.p.ambient = amb;
    c.p.lag = (k % 3 == 0) ? 3 : (k % 3 == 1) ? 8 : 30;
    c.p.resolution = (k % 4 < 2) ? 0.1 : 0.01;
    c.label = "full" + std::to_string(k);
    out.push_back(c); ++k;
  }
  return out;
}

}  // namespace

int main(int argc, char **argv) {
  const std::string mode = argc > 1 ? argv[1] : "mini";
  std::ofstream csv(argc > 2 ? argv[2] : "/dev/null");
  const unsigned shard = argc > 3 ? static_cast<unsigned>(std::atoi(argv[3])) : 0U;
  const unsigned shards = argc > 4 ? static_cast<unsigned>(std::atoi(argv[4])) : 1U;
  std::vector<Case> cases = mode == "full" ? fullCases() : miniCases();
  if (mode.compare(0, 4, "one:") == 0) {   // one:eff,capacity,loss,dead,ambient,resolution[,lag]
    Case c; double lag = 8;
    if (std::sscanf(mode.c_str() + 4, "%lf,%lf,%lf,%lf,%lf,%lf,%lf", &c.p.eff, &c.p.capacity, &c.p.loss, &c.p.dead, &c.p.ambient, &c.p.resolution, &lag) < 6) return 2;
    c.p.lag = lag; c.label = "one"; cases.assign(1, c);
  }
  if (std::getenv("TUNE_TRACE")) serialEnabled = true;
  csv << std::fixed << std::setprecision(5);
  csv << "label,eff,capacity,loss,dead,lag,ambient,resolution,reach,class,reason,rejection,last_phase,tune_s,peak,high_tune,emergency_tune,"
         "kp,ki,kd,gain,gain_true,gain_err_pct,delay,delay_true,delay_err_s,coast100,coast_true,coast_err_c,hold,hold_true,ku,pu,confidence,"
         "ev_over,ev_mae,ev_p95,ev_ripple,post_over,post_mae,post_p95,post_ripple,post_settle,post_high,post_emergency,verdict,val_why,val_res,val_horizon_s,val_tail_s,val_hold\n";
  unsigned n = 0, accepted = 0, bad = 0, started = 0, model = 0, candidate = 0, validated = 0, timeouts = 0, safety = 0, sensor = 0, rejected = 0;
  unsigned highAny = 0, emergencyAny = 0, postHigh = 0, postEmergency = 0;
  for (size_t idx = 0; idx < cases.size(); ++idx) {
    if (idx % shards != shard) continue;
    const Case &c = cases[idx];
    ++n;
    diagnosticLines.clear();
    if (std::getenv("BASELINE")) {   // context: the same plant under Adaptive V1 with the DEFAULT gains and no AutoTune
      Scenario b; b.sp = c.sp; b.durationS = 10800; b.mode = Mode::Adaptive; b.label = c.label;
      const Result r0 = run(c.p, b);
      std::fprintf(stderr, "BASE %s reach=%s over=%.2f mae=%.3f p95=%.3f ripple=%.2f settle=%d high=%d em=%d\n", c.label.c_str(), reachName(r0.reach), r0.overshoot, r0.mae, r0.p95, r0.ripple, r0.settling, r0.high, r0.emergency);
    }
    TuneOpts opts;
    const TuneOut o = tune(c.p, c.sp, opts);
    if (std::getenv("TUNE_TRACE")) for (const std::string &l : diagnosticLines) if (l.compare(0, 5, "[TUNE") == 0) std::fprintf(stderr, "%s", l.c_str());
    if (const char *rc = std::getenv("TUNE_RELAY_CSV")) {   // measurement only: output edges during the tune (SSR / master contactor / exhaust / circulation)
      FILE *rf = std::fopen(rc, "a");
      if (rf) { std::fprintf(rf, "%s,%d,%.0f,%u,%u,%u,%u,%u\n", c.label.c_str(), o.accepted ? 1 : 0, o.tuneS, o.ssrEdges, o.masterEdges, o.ventEdges, o.circEdges, o.maxSsrEdges10min); std::fclose(rf); }
    }
    if (o.started) ++started;
    if (o.modelDone) ++model;
    if (o.candidate) ++candidate;
    if (o.validated) ++validated;
    const std::string cls = classOf(o);
    if (cls == "TIMEOUT") ++timeouts; else if (cls == "SAFETY_ABORT") ++safety; else if (cls == "SENSOR_ABORT") ++sensor; else if (cls == "REJECTED") ++rejected;
    if (o.high) ++highAny;
    if (o.emergency) ++emergencyAny;
    Result post; bool haveCandidateResult = false;
    std::string verdict = cls;
    if (o.accepted) {
      ++accepted;
      Scenario sc; sc.sp = c.sp; sc.durationS = 10800; sc.mode = Mode::Adaptive;
      sc.gainsSet = true; sc.kp = o.kp; sc.ki = o.ki; sc.kd = o.kd;
#ifndef LEGACY_ENGINE
      sc.seedSet = !std::getenv("NO_SEED"); sc.seed = o.seed;
      if (std::getenv("SEED_CONF")) sc.seed.confidence = static_cast<uint8_t>(std::atoi(std::getenv("SEED_CONF")));
#endif
      sc.label = c.label;
      if (std::getenv("POST_TRACE")) { sc.trace = true; sc.traceOut = &std::cerr; }
      if (std::getenv("POST_DEBUG")) { sc.debugOut = &std::cerr; sc.debugFrom = 3000; sc.debugTo = 3100; }
      post = run(c.p, sc);
      if (std::getenv("POST_TRACE")) std::fprintf(stderr, "POST mismatchEvents=%u kh=%u hold=%u guards=%u conf=%d state=%d gain=%.4f delay=%.1f hold=%.1f\n", post.mismatchEvents, post.mismatchKh, post.mismatchHold, post.overshootGuards, post.confidence, post.state, post.gainEst, post.delayEst, post.holdEst);
      haveCandidateResult = true;
      const Reach reach = classify(c.p, c.sp);
      const bool safetyBad = post.high > 0 || post.emergency > 0 || o.high || o.emergency;
      const bool perfBad = reach == Reach::Reachable &&
          !(post.overshoot <= 0.30 && post.mae <= 0.12 && post.p95 <= 0.18 && post.ripple <= 0.30 && post.settling >= 0);
      if (post.high > 0) ++postHigh;
      if (post.emergency > 0) ++postEmergency;
      verdict = (safetyBad || perfBad) ? "ACCEPTED_BAD" : "ACCEPTED_GOOD";
      if (safetyBad || perfBad) ++bad;
    }
    const Plant &p = c.p;
    const double ghTrue = p.khTrue();
    csv << c.label << ',' << p.eff << ',' << p.capacity << ',' << p.loss << ',' << p.dead << ',' << p.lag << ',' << p.ambient << ',' << p.resolution
        << ',' << reachName(classify(p, c.sp)) << ',' << cls << ',' << autoTuneReasonName(o.reason) << ',' << autoTuneReasonName(o.rejection)
        << ',' << autoTunePhaseName(o.lastPhase) << ',' << o.tuneS << ',' << o.peak << ',' << o.high << ',' << o.emergency
        << ',' << o.kp << ',' << o.ki << ',' << o.kd << ',' << o.gain << ',' << ghTrue << ',' << (o.gain > 0 ? 100.0 * (o.gain - ghTrue) / ghTrue : 0.0)
        << ',' << o.delay << ',' << (p.dead + p.lag) << ',' << (o.delay > 0 ? o.delay - (p.dead + p.lag) : 0.0)
        << ',' << o.coast100 << ',' << coastTruthC(p) << ',' << (o.coast100 > 0 ? o.coast100 - coastTruthC(p) : 0.0)
        << ',' << o.hold << ',' << holdTruthPct(p, c.sp) << ',' << o.ku << ',' << o.pu << ',' << unsigned(o.confidence)
        << ',' << o.evOver << ',' << o.evMae << ',' << o.evP95 << ',' << o.evRipple;
    if (haveCandidateResult)
      csv << ',' << post.overshoot << ',' << post.mae << ',' << post.p95 << ',' << post.ripple << ',' << post.settling << ',' << post.high << ',' << post.emergency;
    else csv << ",,,,,,,";
    csv << ',' << verdict;
#ifndef LEGACY_ENGINE
    csv << ',' << (o.why[0] ? o.why : "-") << ',' << o.valRes << ',' << o.valHorizonS << ',' << o.valTailS << ',' << o.valHold;
#else
    csv << ",-,0,0,0,0";
#endif
    csv << '\n';
    std::fprintf(stderr, "%s eff=%.1f cap=%.0f loss=%.0f dead=%.0f amb=%.0f res=%.2f -> %s %s/%s phase=%s t=%.0fs %s\n", c.label.c_str(), p.eff, p.capacity, p.loss,
                 p.dead, p.ambient, p.resolution, cls.c_str(), autoTuneReasonName(o.reason), autoTuneReasonName(o.rejection),
                 autoTunePhaseName(o.lastPhase), o.tuneS, verdict.c_str());
  }
  std::printf("SUMMARY mode=%s engine=%s cases=%u started=%u model_valid=%u candidate=%u validation_started=%u accepted=%u rejected=%u timeout=%u safety_abort=%u sensor_abort=%u "
              "ACCEPTED_BAD=%u high_during_tune=%u emergency_during_tune=%u post_high=%u post_emergency=%u\n",
              mode.c_str(),
#ifdef LEGACY_ENGINE
              "legacy",
#else
              "smart",
#endif
              n, started, model, candidate, validated, accepted, rejected, timeouts, safety, sensor, bad, highAny, emergencyAny, postHigh, postEmergency);
  return 0;
}
