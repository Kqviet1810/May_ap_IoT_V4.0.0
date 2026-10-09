// Smart AutoTune V1 unit + rollback + immediate-abort + power-loss qualification (SOFTWARE / SIMULATION ONLY).
#include "thermal-smart-tune-sim.h"

using namespace sim;
using namespace smarttune;

static unsigned checks = 0;
#define CHECK(cond) do { ++checks; if (!(cond)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); std::abort(); } } while (0)

static Plant referencePlant() {   // light, weak heater, no dead time: the quickest accepted plant of the mini matrix
  Plant p; p.eff = 0.5; p.capacity = 180000; p.loss = 120; p.dead = 0; p.lag = 8; p.ambient = 20; p.resolution = 0.01;
  return p;
}
static Plant mediumPlant() {
  Plant p; p.eff = 1.0; p.capacity = 600000; p.loss = 300; p.dead = 0; p.lag = 8; p.ambient = 28; p.resolution = 0.1;
  return p;
}

// ---- A. step estimator on synthetic ramps ---------------------------------------------------------------------------
static void estimatorTests() {
  struct Case { double k, theta, duty, res; };
  const Case cases[] = {{0.04, 25, 0.25, 0.01}, {0.01, 60, 0.25, 0.1}, {0.10, 10, 0.25, 0.1}, {0.02, 40, 0.35, 0.01}, {0.005, 90, 0.35, 0.01}};
  for (const Case &c : cases) {
    SmartTune::StepEstimator est;
    est.reset(20.0f, 1000U, static_cast<float>(c.duty));
    for (unsigned n = 0; n < 400; ++n) {
      const double t = n * 2.0;
      const double eDelayed = c.duty * std::max(0.0, t - c.theta);   // ideal delayed ON-seconds
      const double pv = 20.0 + c.k * eDelayed;
      est.sample(1000U + n * 2000U, static_cast<float>(std::round(pv / c.res) * c.res));
      if (t > 40 + 2.5 * c.theta && c.k * eDelayed > 0.6) {
        const SmartTune::StepEstimator::Fit f = est.fit();
        CHECK(f.valid);
        CHECK(std::fabs(f.gain - c.k) / c.k < 0.12);
        CHECK(std::fabs(f.delay - c.theta) < std::max(8.0, 0.30 * c.theta));
        break;
      }
    }
  }
  // no response: never a usable fit
  SmartTune::StepEstimator flat;
  flat.reset(20.0f, 1000U, 0.25f);
  for (unsigned n = 0; n < 300; ++n) flat.sample(1000U + n * 2000U, 20.0f);
  const SmartTune::StepEstimator::Fit f = flat.fit();
  CHECK(!f.valid || f.gain < 1e-5f || f.rise < 0.05f);
}

// ---- B. SIMC candidate ---------------------------------------------------------------------------------------------------
static void simcTests() {
  float kp, ki, tauI;
  // gain 0.04 C/s per 100 %, apparent delay 24 s: theta = 28, tauC = 42, Kc = 1/(4e-4*70) = 35.7, tauI = 280
  CHECK(SmartTune::simc(0.04f, 24.0f, 0.0f, 0.0f, kp, ki, tauI));
  CHECK(std::fabs(kp - 35.714f) < 0.1f && std::fabs(tauI - 280.0f) < 0.1f && std::fabs(ki - kp / tauI) < 1e-4f);
  // the relay's ultimate gain caps Kp at Ku / 2.2
  CHECK(SmartTune::simc(0.04f, 24.0f, 44.0f, 0.0f, kp, ki, tauI) && std::fabs(kp - 20.0f) < 0.01f);
  // hard bounds
  CHECK(SmartTune::simc(0.002f, 24.0f, 0.0f, 0.0f, kp, ki, tauI) && kp <= SmartTune::KpMax && ki >= SmartTune::KiMin && ki <= SmartTune::KiMax);
  CHECK(!SmartTune::simc(20.0f, 2.0f, 0.0f, 0.0f, kp, ki, tauI));           // absurdly strong heater: Kp < 1 is refused
  CHECK(!SmartTune::simc(NAN, 24.0f, 0.0f, 0.0f, kp, ki, tauI));
  CHECK(!SmartTune::simc(0.04f, INFINITY, 0.0f, 0.0f, kp, ki, tauI));
  CHECK(!SmartTune::simc(0.0f, 24.0f, 0.0f, 0.0f, kp, ki, tauI));
  CHECK(!SmartTune::simc(0.04f, 24.0f, 40.0f, 20.0f, kp, ki, tauI));        // relay period << 2 theta: inconsistent model
  // the candidate keeps Ki equal to V1's own stability ceiling kiLimit = 100 / (16 Kh theta^2) when tauC = theta... and never above it
  for (float g : {0.01f, 0.03f, 0.08f}) for (float d : {10.0f, 30.0f, 80.0f}) {
    if (!SmartTune::simc(g, d, 0.0f, 0.0f, kp, ki, tauI)) continue;
    const float theta = std::max(6.0f, d + 4.0f);
    CHECK(ki <= 100.0f / (16.0f * g * theta * theta) * 1.001f + SmartTune::KiMin);
  }
}

// ---- C. start preconditions + baseline gates -------------------------------------------------------------------------------
static void preconditionTests() {
  SmartAutoTune t;
  MachineConfig cfg;
  const char *msg = "";
  CHECK(!t.preflight(NAN, cfg, msg));
  CHECK(!t.preflight(cfg.targetTemp - 1.0f, cfg, msg));       // warm chamber: no room for the first excitation
  CHECK(t.preflight(cfg.targetTemp - 10.0f, cfg, msg));
  // jumpy baseline
  for (int variant = 0; variant < 3; ++variant) {
    SmartAutoTune b;
    b.configure(37.5f);
    b.start(1000U, 25.0f);
    MachineConfig tuned;
    for (unsigned n = 1; n < 80 && b.running(); ++n) {
      float pv = 25.0f;
      if (variant == 0 && n == 20) pv = 26.0f;                  // 1 C jump
      if (variant == 1) pv = 25.0f + 0.01f * n;                  // 0.005 C/s drift
      if (variant == 2) pv = 25.0f + ((n & 1U) ? 0.2f : -0.2f); // noisy
      b.tick(1000U + n * 2000U, false);
      b.update(1000U + n * 2000U, pv, cfg, tuned);
    }
    CHECK(b.state() == AutoTuneState::Failed && b.reason() == AutoTuneReason::BaselineUnstable && b.power() == 0.0f);
  }
  // a quiet baseline proceeds to the first excitation (25 %, capped by maxHeaterPower)
  SmartAutoTune q;
  q.configure(37.5f);
  q.start(1000U, 25.0f);
  MachineConfig tuned;
  for (unsigned n = 1; n < 60; ++n) { q.tick(1000U + n * 2000U, false); q.update(1000U + n * 2000U, 25.0f, cfg, tuned); }
  CHECK(q.running() && q.phase() == AutoTunePhase::Excite && q.power() == 25.0f);
  cfg.maxHeaterPower = 15;
  SmartAutoTune c;
  c.configure(37.5f);
  c.start(1000U, 25.0f);
  for (unsigned n = 1; n < 60; ++n) { c.tick(1000U + n * 2000U, false); c.update(1000U + n * 2000U, 25.0f, cfg, tuned); }
  CHECK(c.power() == 15.0f);
}

// ---- D. bumpless take-over ---------------------------------------------------------------------------------------------------
static void bumplessTests() {
  ThermalController pid;
  MachineConfig cfg; cfg.kp = 20.0f; cfg.ki = 0.1f; cfg.kd = 0.0f; cfg.controlMode = ControlMode::Pid;
  pid.seedBumpless(5000U, 37.5f, 37.3f, cfg, 25.0f);
  CHECK(std::fabs(pid.output() - 25.0f) < 1e-3f && std::fabs(pid.integral() - (25.0f - 20.0f * 0.2f)) < 1e-3f);
  const float first = pid.updateOnNewSample(7000U, 37.5f, 37.3f, cfg, true);
  CHECK(std::fabs(first - 25.0f) < 1.0f);                       // no heater step at the hand-over
  ThermalController off;
  off.seedBumpless(5000U, 37.5f, NAN, cfg, 25.0f);
  CHECK(off.output() == 0.0f && off.integral() == 0.0f);        // invalid PV: nothing is carried over
}

// ---- E. complete tune: accept, atomic save, profile seed + persistence -------------------------------------------------------
static void acceptTests() {
  TuneOpts o;
  const Plant p = referencePlant();
  const TuneOut r = tune(p, 37.5f, o);
  CHECK(r.started && r.accepted && r.reason == AutoTuneReason::Success);
  CHECK(r.saves == 1 && r.oldKept);                                  // exactly one atomic config record
  CHECK(!r.high && !r.emergency);
  CHECK(std::fabs(r.gain - p.khTrue()) / p.khTrue() < 0.12);          // identified effective plant gain
  CHECK(std::fabs(r.delay - (p.dead + p.lag)) < 8.0f);
  CHECK(std::fabs(r.hold - holdTruthPct(p, 37.5)) < 3.0);             // relay mean duty = hold at the setpoint
  CHECK(r.kp > 1.0f && r.kp <= SmartTune::KpMax && r.ki >= SmartTune::KiMin && r.kd == 0.0f);
  CHECK(r.seed.confidence >= 60 && r.seed.confidence <= 75);          // measured: above the aged-seed cap (40), never 100
  CHECK(r.profileSaves >= 1);                                         // the profile reached flash through the single writer
  // the persisted profile is a valid record that a new boot would load as a seed
  MayapThermal::profileStorage.resetForTest();
  MayapThermal::profileStorage.service(1000U);
  MayapThermal::ThermalProfile loaded{};
  CHECK(MayapThermal::profileStorage.takeSeed(loaded) && MayapThermal::validProfile(loaded));
  CHECK(std::fabs(loaded.heaterGain - r.seed.heaterGain) < 1e-6f);
}

// ---- F. immediate abort at every phase, for every fault; rollback is total --------------------------------------------------
static void abortTests() {
  const Plant p = referencePlant();
  TuneOpts ref;
  const TuneOut base = tune(p, 37.5f, ref);
  CHECK(base.accepted);
  const AutoTunePhase phases[] = {AutoTunePhase::Baseline, AutoTunePhase::Excite, AutoTunePhase::Coast, AutoTunePhase::Approach,
                                  AutoTunePhase::NearSp, AutoTunePhase::Settle, AutoTunePhase::Validating};
  const Fault faults[] = {Fault::SensorLost, Fault::SensorInvalid, Fault::SensorFrozen, Fault::AutoOff, Fault::HeaterOff, Fault::Batch,
                          Fault::Inhibit, Fault::Trip, Fault::Maintenance, Fault::HighTemp, Fault::StorageFault};
  unsigned cuts = 0;
  for (AutoTunePhase ph : phases) {
    const double at = ref.phaseFirstS[static_cast<unsigned>(ph)];
    if (at < 0) continue;                      // (Settle can be skipped when PV is already at the setpoint)
    for (Fault f : faults) {
      TuneOpts o;
      o.fault = f; o.faultAtS = at + 6.0;
      const TuneOut r = tune(p, 37.5f, o);
      CHECK(r.started && !r.accepted);
      CHECK(r.oldKept && r.saves == 0 && r.profileSaves == 0);   // old PID and profile exactly as before
      CHECK(r.reason == AutoTuneReason::SafetyAbort || r.reason == AutoTuneReason::SensorAbort || r.reason == AutoTuneReason::ModeAbort);
      CHECK(!r.high && !r.emergency);
      ++cuts;
    }
    TuneOpts oc;                                // operator cancel
    oc.fault = Fault::Cancel; oc.faultAtS = at + 6.0;
    const TuneOut rc = tune(p, 37.5f, oc);
    CHECK(rc.started && !rc.accepted && rc.oldKept && rc.saves == 0);
    ++cuts;
  }
  CHECK(cuts >= 6U * 12U);
  std::printf("  immediate-abort matrix: %u cuts, heater OFF within one control cycle, no save, old PID/profile intact\n", cuts);
}

// ---- G. power loss at every phase ----------------------------------------------------------------------------------------------
static void powerLossTests() {
  const Plant p = referencePlant();
  TuneOpts ref;
  CHECK(tune(p, 37.5f, ref).accepted);
  unsigned n = 0;
  for (unsigned i = 0; i < 16U; ++i) {
    if (ref.phaseFirstS[i] < 0) continue;
    const AutoTunePhase ph = static_cast<AutoTunePhase>(i);
    if (ph == AutoTunePhase::Idle || ph == AutoTunePhase::Failed || ph == AutoTunePhase::Success || ph == AutoTunePhase::Generate) continue;
    TuneOpts o;
    o.rebootAtS = ref.phaseFirstS[i] + 8.0;
    const TuneOut r = tune(p, 37.5f, o);
    CHECK(o.rebootChecked && r.oldKept && !r.accepted);
    ++n;
  }
  CHECK(n >= 7U);
  // power cut INSIDE the config save: A/B slots + CRC keep the previous record, the tune reports failure, nothing is applied
  for (int budget : {0, 1, 8, 40, 100}) {
    TuneOpts o;
    o.saveBudget = budget;
    const TuneOut r = tune(p, 37.5f, o);
    CHECK(r.started && !r.accepted && r.reason == AutoTuneReason::SaveFailed);
    CHECK(r.profileSaves == 0);                               // no profile is written for a tune that was not saved
    CHECK(r.oldKept || r.saves >= 1);                         // (the failed write is counted; the stored record is still the old one)
  }
  std::printf("  power-loss: %u phase boundaries + 5 save truncations, old PID/profile intact\n", n);
}

// ---- H. validation rejects (not an abort): rollback ---------------------------------------------------------------------------
static void validationRejectTests() {
  // light plant, 0.1 C probe: the candidate cannot hold the tail band (p95/ripple of one probe step) -> NOT accepted, nothing saved
  Plant p; p.eff = 0.7; p.capacity = 180000; p.loss = 180; p.dead = 15; p.lag = 8; p.ambient = 20; p.resolution = 0.1;
  TuneOpts o;
  const TuneOut r = tune(p, 37.5f, o);
  CHECK(r.started && !r.accepted && r.oldKept && r.saves == 0 && r.profileSaves == 0);
  CHECK(r.reason == AutoTuneReason::ValidationFailed && r.candidate && r.validated);   // a candidate existed and was validated, then refused
  // a plant that cannot hold its setpoint with margin (hold > 30 %) is classified HEAT_LIMITED, not tuned
  Plant weak; weak.eff = 0.5; weak.capacity = 600000; weak.loss = 300; weak.dead = 0; weak.ambient = 20; weak.resolution = 0.1;
  TuneOpts ow;
  const TuneOut rw = tune(weak, 37.5f, ow);
  CHECK(!rw.accepted && rw.reason == AutoTuneReason::PowerLimited && rw.oldKept);
  // a warm chamber is refused at the start
  Plant warm = referencePlant(); warm.ambient = 36.5;
  TuneOpts owarm;
  const TuneOut rr = tune(warm, 37.5f, owarm);
  CHECK(!rr.started);
}


// ---- J. verification horizon, probe resolution and regime evidence (targeted regressions of the two full-matrix escapes) --------
static void feedRaw(SmartAutoTune &a, const std::vector<float> &raws) {
  for (float r : raws) a.noteSensor(r, true, false);
}
static void horizonPlannerTests() {
  SmartTune::VerifyPlan pl;
  // a quick oven keeps the original 45 min check (15 min tail): the verification is not slower than it used to be
  CHECK(SmartTune::verificationPlan(150.0f, 118.0f, 0.01f, 3600000U, pl));
  CHECK(pl.totalMs == 2700000U && pl.tailMs == 900000U && pl.warmMs == 1800000U);
  // a slow loop (tauI 648 s = delay 60 s) needs a longer window: warm-up >= 3 tauI, tail >= 2 tauI (and 2.5 relay periods)
  CHECK(SmartTune::verificationPlan(648.0f, 728.0f, 0.01f, 3600000U, pl));
  CHECK(pl.warmMs >= 3U * 648000U && pl.tailMs >= 2U * 648000U && pl.tailMs >= 2.5f * 728000.0f - 1.0f && pl.totalMs > 2700000U);
  const uint32_t fineTail = pl.tailMs;
  // a coarse probe (0.1 C) stretches the tail by 1.5
  CHECK(SmartTune::verificationPlan(648.0f, 728.0f, 0.1f, 3600000U, pl));
  CHECK(pl.tailMs == static_cast<uint32_t>(fineTail * 1.5f));
  // more than the hard maximum is refused outright (never shortened), and so is a window the tune no longer has time for
  CHECK(!SmartTune::verificationPlan(4300.0f, 2000.0f, 0.01f, 3600000U, pl));
  CHECK(SmartTune::verificationPlan(150.0f, 118.0f, 0.01f, 3600000U, pl));
  CHECK(!SmartTune::verificationPlan(150.0f, 118.0f, 0.01f, SmartTune::TotalMaxMs - 2700000U, pl));
  CHECK(!SmartTune::verificationPlan(NAN, 100.0f, 0.01f, 0U, pl) && !SmartTune::verificationPlan(0.0f, 100.0f, 0.01f, 0U, pl));
  // the hard maximum is bounded
  CHECK(SmartTune::ValidateHardMaxMs <= 9000000UL);
}
static void resolutionEstimatorTests() {
  const double ress[] = {0.01, 0.1, 0.05, 0.2};
  for (double res : ress) {
    SmartAutoTune a; a.configure(37.5f); a.start(1000U, 20.0f);
    std::vector<float> raws;
    for (unsigned n = 0; n < 600; ++n) raws.push_back(static_cast<float>(std::round((20.0 + 0.004 * n + 0.0013 * (n % 7)) / res) * res));
    feedRaw(a, raws);
    CHECK(std::fabs(a.resolution() - res) < 0.002);
  }
  { SmartAutoTune a; a.configure(37.5f); a.start(1000U, 20.0f);       // no step at all (flat probe): nothing is proven -> coarsest class
    feedRaw(a, std::vector<float>(300, 20.0f)); CHECK(a.resolution() >= 0.5f); }
  { SmartAutoTune a; a.configure(37.5f); a.start(1000U, 20.0f);       // too few steps to trust
    feedRaw(a, {20.0f, 20.0f, 20.1f, 20.1f, 20.2f, 20.2f, 20.3f, 20.3f}); CHECK(a.resolution() >= 0.5f);
    feedRaw(a, {20.4f, 20.5f}); CHECK(std::fabs(a.resolution() - 0.1f) < 0.002f); }
  { SmartAutoTune a; a.configure(37.5f); a.start(1000U, 20.0f);       // float dust below the noise floor is not a quantum
    std::vector<float> raws; for (unsigned n = 0; n < 400; ++n) raws.push_back(20.0f + ((n % 2) ? 0.0004f : 0.0f) + (n / 40) * 0.1f);
    feedRaw(a, raws); CHECK(std::fabs(a.resolution() - 0.1f) < 0.002f); }
  { SmartAutoTune a; a.configure(37.5f); a.start(1000U, 20.0f);       // stale (not fresh) readings and NaN are ignored
    for (unsigned n = 0; n < 100; ++n) a.noteSensor(20.0f + n * 0.01f, false, false);
    a.noteSensor(NAN, true, false); CHECK(a.resolution() >= 0.5f); }
}
static void verificationRegressionTests() {
  // (1) the former full344: slow loop (delay ~60 s), 0.1 C probe, PV parked one probe step under the setpoint. The old 15 min check
  //     read MAE 0.100 and passed it; the true error could be 0.15. Now: longer window AND worst-case resolution margin -> refused.
  {
    Plant p; p.eff = 1.2; p.capacity = 1600000; p.loss = 180; p.dead = 30; p.lag = 30; p.ambient = 20; p.resolution = 0.1;
    TuneOpts o; const TuneOut r = tune(p, 37.5f, o);
    CHECK(r.started && !r.accepted && r.oldKept && r.saves == 0 && r.profileSaves == 0 && !r.high && !r.emergency);
    CHECK(r.reason == AutoTuneReason::ValidationFailed && r.validated);
    CHECK(std::strcmp(r.why, "mae") == 0 && std::fabs(r.valRes - 0.1f) < 0.003f);
    CHECK(r.valHorizonS > 2700U && r.valTailS > 900U);                 // the window was stretched for the slow loop
    CHECK(r.evMae <= 0.12f && r.evMae + 0.5f * r.valRes > 0.12f);       // it WOULD have passed on the measured number alone
  }
  // (2) the former full400: 0.1 C probe, the controller only reached its steady HOLD regime late in the window. Not enough evidence.
  {
    Plant p; p.eff = 1.5; p.capacity = 600000; p.loss = 180; p.dead = 0; p.lag = 8; p.ambient = 20; p.resolution = 0.1;
    TuneOpts o; const TuneOut r = tune(p, 37.5f, o);
    CHECK(r.started && !r.accepted && r.oldKept && r.saves == 0 && !r.high && !r.emergency);
    CHECK(std::strcmp(r.why, "not-steady") == 0 && r.valHold < SmartTune::ValidHoldFrac);
    CHECK(r.evMae + 0.5f * r.valRes <= 0.12f);                          // every numeric target looked fine: the refusal is the missing regime evidence
  }
  // (3) fast, fine probe: still accepted, same 45 min check, not slower than before (4290 s on this plant before the change)
  {
    TuneOpts o; const TuneOut r = tune(referencePlant(), 37.5f, o);
    CHECK(r.accepted && r.saves == 1 && r.valHorizonS == 2700U && r.valTailS == 900U && std::fabs(r.valRes - 0.01f) < 0.002f);
    CHECK(r.tuneS <= 4290.0 + 60.0 && r.valHold >= SmartTune::ValidHoldFrac);
  }
  // (4) medium oven (fine probe, delay ~13 s) and a medium oven with a 0.1 C probe that holds the tail band: both still accepted
  {
    Plant p; p.eff = 1.0; p.capacity = 600000; p.loss = 180; p.dead = 15; p.lag = 3; p.ambient = 20; p.resolution = 0.01;
    TuneOpts o; const TuneOut r = tune(p, 37.5f, o);
    CHECK(r.accepted && r.saves == 1 && !r.high && !r.emergency);
    Plant q = p; q.dead = 0; q.lag = 8; q.resolution = 0.1;
    TuneOpts o2; const TuneOut r2 = tune(q, 37.5f, o2);
    CHECK(r2.accepted && r2.saves == 1 && r2.valTailS > 900U && !r2.high && !r2.emergency);   // coarse probe: longer tail
  }
  // (5) power-limited keeps its own reason; nothing reaches the verification
  {
    Plant weak; weak.eff = 0.5; weak.capacity = 600000; weak.loss = 300; weak.dead = 0; weak.ambient = 20; weak.resolution = 0.1;
    TuneOpts o; const TuneOut r = tune(weak, 37.5f, o);
    CHECK(!r.accepted && r.reason == AutoTuneReason::PowerLimited && r.oldKept && !r.validated);
  }
}

// ---- I. matrix of other plants: nothing accepted may be bad, nothing may cross High/Emergency -----------------------------
static void safetyMatrixTests() {
  const double effs[] = {0.7, 1.0, 1.5};
  const double caps[] = {180000, 600000};
  const double deads[] = {0, 30};
  unsigned accepted = 0, total = 0;
  for (double eff : effs) for (double cap : caps) for (double dead : deads) {
    Plant p; p.eff = eff; p.capacity = cap; p.loss = 180; p.dead = dead; p.lag = 8; p.ambient = 24; p.resolution = 0.01;
    TuneOpts o;
    const TuneOut r = tune(p, 37.5f, o);
    ++total;
    CHECK(!r.high && !r.emergency);
    CHECK(r.oldKept);
    if (r.accepted) { ++accepted; CHECK(r.saves == 1); }
  }
  CHECK(accepted >= 3U);
  std::printf("  safety matrix: %u/%u accepted, High=0 Emergency=0, every rejection rolled back\n", accepted, total);
}

int main() {
  estimatorTests();
  simcTests();
  preconditionTests();
  bumplessTests();
  acceptTests();
  abortTests();
  powerLossTests();
  validationRejectTests();
  safetyMatrixTests();
  horizonPlannerTests();
  resolutionEstimatorTests();
  verificationRegressionTests();
  (void)mediumPlant;
  std::printf("Smart AutoTune V1: estimator, SIMC bounds, baseline gates, bumpless hand-over, accept+atomic save+profile seed, immediate aborts at every phase, power loss, rejection rollback (%u checks) PASS\n", checks);
  return 0;
}
