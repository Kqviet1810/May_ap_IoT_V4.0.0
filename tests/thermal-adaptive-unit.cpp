// Adaptive Thermal V1 unit tests (host). Pure logic: profile, learner, planner, PID assist.
// The closed-loop evidence lives in thermal-adaptive-v1.cpp; these tests pin the contracts.
#include "thermal-fixture.h"
#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_control.h"
#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_adaptive_v1.h"
#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_profile.h"
#include <chrono>
#include <cstring>
#include <vector>

using namespace MayapThermal;

static int checks = 0;
#define CHECK(cond) do { ++checks; if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); std::abort(); } } while (0)

// ---------------------------------------------------------------------------------------------
// Open-loop rig: a known aperiodic ACTUAL duty pattern drives dead time + lag + heat capacity,
// sampled at 0.1 C resolution through a light IIR (what the production filter looks like).
// ---------------------------------------------------------------------------------------------
struct Rig {
  double eff = 1.0, cap = 180000, loss = 120, dead = 30, lag = 8, res = 0.1;
  double startMs = 1000;       // first `now` (set near 2^32 to test rollover)
  double hours = 3.0;
  double effAfter = -1, changeAtH = 1e9;  // plant change (heater effectiveness)
  bool blocked = false;        // SSR physically inhibited: actual on-time is zero
  bool vent = false;           // exhaust on during [ventFrom, ventTo) seconds, flagged to the learner
  double ventFrom = 0, ventTo = 0;
  double ventEveryS = 0, ventLenS = 120, ventW = 150;  // periodic vent (after ventFrom) instead of one window
  int badEvery = 0;            // sprinkle a NaN then an Inf sample every N samples (0 = never)
  bool tune = false;
  bool closed = false;         // PI loop (sp 40) instead of the fixed dither pattern: forms a real hold
  double sp = 40, integ = 0;
  ThermalLearner L;
  ThermalLearner *Lp = &L;     // learner under test (may be an AdaptiveV1's own)
  float lastPv = NAN; uint32_t lastNow = 0;
  double worstSampleUs = 0, totSampleUs = 0; unsigned nSamples = 0;  // host cost of one learner sample
  double khTrue() const { return 16000.0 * eff / cap; }

  void run() {
    ThermalLearner &L = *Lp;
    L.reset(); L.setEnabled(true);
    const double dt = 0.1;
    std::vector<double> pipe(static_cast<size_t>(dead / dt) + 1, 0.0);
    size_t cur = 0;
    double T = 25, heater = 0, filt = 25, ph = 0;
    int holdTicks = 0;
    static const double lens[] = {300, 200, 420, 260, 380, 150, 520, 240, 330, 180, 460, 280};
    static const double lev[] = {1.0, 0.1, 0.7, 0.0, 0.5, 1.0, 0.2, 0.8, 0.0, 0.6, 0.3, 1.0};
    const unsigned steps = static_cast<unsigned>(hours * 3600 / dt);
    double e = eff;
    for (unsigned k = 0; k < steps; ++k) {
      const double t = k * dt;
      if (t / 3600.0 >= changeAtH && effAfter > 0) e = effAfter;
      const uint32_t now = static_cast<uint32_t>(static_cast<uint64_t>(startMs + t * 1000.0));
      double tt = t, duty = 0; int i = 0;
      while (true) { const double l = lens[i % 12]; if (tt < l) { duty = lev[i % 12]; break; } tt -= l; ++i; }
      if (closed) {
        const double err = sp - filt; integ = std::min(1.0, std::max(0.0, integ + 1e-4 * err * dt));
        duty = std::min(1.0, std::max(0.0, 0.08 * err + integ));
      }
      ph += duty * dt / 0.3; bool pulse = false; if (ph >= 1.0) { ph -= 1.0; pulse = true; }
      if (pulse && !blocked) holdTicks = 3;
      const bool on = holdTicks > 0; if (holdTicks > 0) --holdTicks;
      L.tick(now, on);
      const double delivered = on ? 16000.0 * e : 0.0;
      const double dl = pipe[cur]; pipe[cur] = delivered; cur = (cur + 1) % pipe.size();
      heater += (dl - heater) * dt / lag;
      const bool ventOn = vent && (ventEveryS > 0 ? (t >= ventFrom && std::fmod(t - ventFrom, ventEveryS) < ventLenS) : (t >= ventFrom && t < ventTo));
      T += (heater - loss * (T - 25) - (ventOn ? ventW : 0.0)) * dt / cap;
      if (k % 20 == 0) {
        filt += 0.35 * (std::round(T / res) * res - filt);
        LearnInput in;
        in.pv = static_cast<float>(filt); in.raw = in.pv; in.sp = static_cast<float>(sp); in.high = 500;
        in.sensor = true; in.fanStable = true; in.ventActive = ventOn; in.heaterBlocked = blocked; in.tune = tune;
        if (badEvery && (k / 20) % badEvery == 5) in.pv = NAN;
        if (badEvery && (k / 20) % badEvery == 6) in.pv = INFINITY;
        lastPv = in.pv; lastNow = now;
        Hints h;
        const std::chrono::high_resolution_clock::time_point t0 = std::chrono::high_resolution_clock::now();
        L.sample(now, in, h);
        const double us = std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();
        worstSampleUs = std::max(worstSampleUs, us); totSampleUs += us; ++nSamples;
      }
    }
  }
};

// ---------------------------------------------------------------------------------------------
static void profileTests() {
  CHECK(sizeof(ThermalProfile) == 60);
  ThermalProfile p = defaultProfile();
  CHECK(validProfile(p));
  CHECK(p.confidence == 0);
  // CRC / version / size / magic
  ThermalProfile q = p; q.heaterGain = 0.05f; CHECK(!validProfile(q));          // stale CRC
  sealProfile(q); CHECK(validProfile(q));
  q = p; q.version = 2; sealProfile(q); CHECK(!validProfile(q));
  q = p; q.size = 59; sealProfile(q); CHECK(!validProfile(q));
  q = p; q.magic ^= 1U; sealProfile(q); CHECK(!validProfile(q));
  // every single-bit flip of the stored bytes is rejected by CRC/ranges
  unsigned rejected = 0, flips = 0;
  for (size_t byte = 0; byte < offsetof(ThermalProfile, crc); ++byte)
    for (unsigned bit = 0; bit < 8; ++bit) {
      ThermalProfile c = p; reinterpret_cast<uint8_t *>(&c)[byte] ^= static_cast<uint8_t>(1U << bit);
      ++flips; if (!validProfile(c)) ++rejected;
    }
  CHECK(rejected == flips);
  // NaN / Inf / out-of-range fields never validate; sanitising zeroes the confidence
  const float bad[] = {NAN, INFINITY, -INFINITY, -1.0f, 1e9f};
  for (float b : bad) {
    q = p; q.heaterGain = b; sealProfile(q); CHECK(!validProfile(q));
    q = p; q.holdPowerPct = b; sealProfile(q); CHECK(!validProfile(q));
    q = p; q.ventCoolingGain = b; sealProfile(q); CHECK(!validProfile(q));
  }
  q = p; q.confidence = 90; q.heaterGain = NAN; sanitizeProfile(q);
  CHECK(q.confidence == 0 && std::isfinite(q.heaterGain) && profileRangesValid(q));
  q = p; q.confidence = 250; q.state = 9; sanitizeProfile(q); CHECK(q.confidence == 100 && q.state == 0);
  // compatibility and seed trust
  q = p; q.signature = 77; q.confidence = 80; sealProfile(q);
  CHECK(profileCompatible(q, 77, false));
  CHECK(!profileCompatible(q, 78, false));    // other hardware/config
  CHECK(!profileCompatible(q, 77, true));     // abnormal reset
  q.modelVersion = 99; sealProfile(q); CHECK(!profileCompatible(q, 77, false));
  q.modelVersion = ProfileModelVersion; q.epoch = 1700000000U; sealProfile(q);
  CHECK(seedConfidenceCap(q, 1700000000U + 86400U) == 40);
  CHECK(seedConfidenceCap(q, 1700000000U + 40UL * 86400UL) == 15);
  CHECK(seedConfidenceCap(q, 0) == 20);
  // flash-wear policy
  ThermalProfile live = p; live.confidence = 80; live.heaterGain = 0.05f; live.holdPowerPct = 20;
  CHECK(!shouldPersistProfile(live, p, false, 1000));                 // too soon
  CHECK(shouldPersistProfile(live, p, false, 3600000UL));             // nothing stored yet
  ThermalProfile st = live; CHECK(!shouldPersistProfile(live, st, true, 7200000UL));  // no material change
  live.heaterGain = 0.07f; CHECK(shouldPersistProfile(live, st, true, 7200000UL));    // +40 %
  live = st; live.epoch = 1700000000U; st.epoch = live.epoch;
  CHECK(!shouldPersistProfile(live, st, true, 7200000UL));
  live.epoch += 8UL * 86400UL; CHECK(shouldPersistProfile(live, st, true, 7200000UL));   // weekly age refresh
  CHECK(!shouldPersistProfile(live, st, true, 1000));                                    // still rate-limited
  { ThermalProfile other = st; other.signature = st.signature + 1U; other.epoch = st.epoch;
    CHECK(shouldPersistProfile(other, st, true, 7200000UL));               // compatibility metadata changed
    other = st; other.modelVersion = st.modelVersion + 1U;
    CHECK(shouldPersistProfile(other, st, true, 7200000UL)); }
  live = st; live.confidence = 40; CHECK(!shouldPersistProfile(live, p, false, 7200000UL));  // unqualified
}

static void learnerTests() {
  // Gain / delay / hold-independent learning from ACTUAL on-time.
  const double deads[] = {0, 30, 60};
  for (double d : deads) {
    Rig r; r.dead = d; r.lag = 8; r.run();
    const ThermalProfile &p = r.L.profile();
    const double khErr = std::fabs(p.heaterGain - r.khTrue()) / r.khTrue();
    CHECK(khErr < 0.30);
    CHECK(std::fabs(p.heaterDelaySec - (d + r.lag)) < 25.0);
    CHECK(p.confidence >= 55);
    CHECK(validProfile([&] { ThermalProfile c = p; c.signature = 1; sealProfile(c); return c; }()));
  }
  // The learner knows only the ACTUAL on-time: a weaker real heater is learned as weaker.
  { Rig a; a.eff = 1.0; a.run(); Rig b; b.eff = 0.6; b.run();
    const float ratio = b.L.profile().heaterGain / a.L.profile().heaterGain;
    CHECK(ratio > 0.45f && ratio < 0.80f); }
  // Blocked SSR (inhibit / master not picked up): no learning, no confidence.
  { Rig r; r.blocked = true; r.run();
    CHECK(r.L.profile().confidence == 0);
    CHECK(r.L.gate() == GateReason::HeaterBlocked); }
  // Clean-window gate: an AutoTune running, or a whole run of vent, must not teach heater gain.
  { Rig r; r.tune = true; r.run(); CHECK(r.L.profile().confidence == 0); CHECK(r.L.gate() == GateReason::Tune); }
  // A setpoint edit drops the windows that span both operating points: the gate stays shut for a full
  // 2W of fresh history afterwards instead of reopening on the next sample.
  { Rig r; r.hours = 1.0; r.run(); CHECK(r.L.gate() == GateReason::Open);
    const uint32_t now = r.lastNow + 2000;
    LearnInput in; in.pv = r.lastPv; in.raw = r.lastPv; in.sp = 41.0f; in.high = 500; in.sensor = true; in.fanStable = true;
    Hints h; r.L.sample(now, in, h); CHECK(r.L.gate() == GateReason::Settling);
    r.L.sample(now + 2000, in, h); CHECK(r.L.gate() == GateReason::Settling);
    // still shut after settle + 2W: the longest delay candidate would pair pre-edit actuator history
    uint32_t t = now + 2000;
    for (int i = 0; i < 150; ++i) { t += 2000; r.L.sample(t, in, h); }   // 300 s
    CHECK(r.L.gate() == GateReason::Settling); }
  // Bounds: nothing learned leaves the persistent limits, whatever the plant.
  const double effs[] = {0.2, 2.5};
  for (double e : effs) { Rig r; r.eff = e; r.run(); CHECK(profileRangesValid(r.L.profile())); }
  // NaN / Inf samples are discarded, learning continues, the profile stays valid.
  { Rig r; r.badEvery = 997; r.run();     // rare bad samples: learning proceeds
    CHECK(profileRangesValid(r.L.profile()));
    CHECK(r.L.profile().confidence >= 40);
    CHECK(std::isfinite(r.L.profile().heaterGain) && std::isfinite(r.L.profile().heaterDelaySec)); }
  { Rig r; r.badEvery = 40; r.run();      // a sensor that keeps failing: nothing is learned (conservative)
    CHECK(profileRangesValid(r.L.profile()));
    CHECK(r.L.profile().confidence <= 30); }
  // millis() rollover in the middle of the run changes nothing.
  { Rig a; a.run(); Rig b; b.startMs = 4294967296.0 - 3600.0 * 1000.0; b.run();
    CHECK(std::fabs(a.L.profile().heaterGain - b.L.profile().heaterGain) < 0.15f * a.L.profile().heaterGain);
    CHECK(b.L.profile().confidence >= 50); }
  // Seeding from storage is capped and never raises confidence above the cap.
  { Rig a; a.run(); ThermalProfile s = a.L.profile(); s.confidence = 95; s.ventConfidence = 95; sealProfile(s);
    ThermalLearner L; L.setEnabled(true); L.seed(s, 40);
    CHECK(L.profile().confidence <= 40);
    CHECK(L.holdWindows() == 0 && L.holdTrust() == 0.0f);
    CHECK(L.profile().ventConfidence == 0 && L.ventTrust() == 0.0f);   // nor is a stored vent estimate   // stored hold is not a live window
    ThermalProfile badSeed = s; badSeed.heaterGain = NAN;
    ThermalLearner M; M.setEnabled(true); M.seed(badSeed, 40);
    CHECK(M.profile().confidence == 0); }  // an invalid seed is ignored: defaults, confidence 0
  // Plant change (heater 100 % -> 40 %): the profile must follow and confidence must not stay at peak.
  { Rig r; r.hours = 4.0; r.effAfter = 0.4; r.changeAtH = 2.0; r.run();
    const float g = r.L.profile().heaterGain;
    CHECK(g < r.khTrue() * 0.8f);   // moved towards the new plant
    CHECK(r.L.mismatchEvents() >= 1 || g < r.khTrue() * 0.55f); }
}

// A planner whose learner has been TRAINED by the closed-loop rig (a seeded profile is not
// evidence, so it would correctly grant nothing).
static void train(AdaptiveV1 &a, double hours, bool vents) {
  Rig r; r.Lp = &a.learner(); r.closed = true; r.loss = 267; r.hours = hours;   // hold ~25 %
  if (vents) { r.vent = true; r.ventFrom = 5400; r.ventEveryS = 1800; r.ventLenS = 240; r.ventW = 1500; }
  r.run();
}
static const uint32_t trainedNow = 1000U + 8U * 3600U * 1000U;

static void plannerTests() {
  // Unknown oven: legacy behaviour exactly (no authority anywhere).
  { AdaptiveV1 a; a.setEnabled(true);
    const Plan &pl = a.update(100000, true, false, VentInfo{}, 37.0f, 37.5f, 0.0f);
    CHECK(!pl.hint.valid && pl.assist.feedForward == 0 && pl.assist.addForward == 0);
    CHECK(std::isinf(pl.assist.kiMax) && std::isinf(pl.assist.corrBase) && !pl.assist.freezeIntegral); }
  // A stored profile is only a seed: with no fresh evidence it grants nothing, whatever it claims.
  { ThermalProfile p = defaultProfile(); p.heaterGain = 0.05f; p.heaterDelaySec = 30; p.holdPowerPct = 20;
    p.ventCoolingGain = 0.01f; p.confidence = 95; p.ventConfidence = 95; sealProfile(p);
    AdaptiveV1 a; a.learner().seed(p, 40); a.setEnabled(true);
    const Plan &pl = a.update(100000, true, false, VentInfo{}, 37.0f, 37.5f, 0);
    CHECK(pl.assist.feedForward == 0 && pl.assist.addForward == 0 && !pl.hint.valid); }
  // Trained planner: hold feed-forward bounded by the learned hold, gated by permit/tune/NaN/disable.
  { AdaptiveV1 a; train(a, 4.0, false);
    const float hold = a.learner().profile().holdPowerPct;
    CHECK(a.learner().profile().confidence >= 40 && hold > 15.0f);
    const Plan &pl = a.update(trainedNow, true, false, VentInfo{}, 39.9f, 40.0f, 0);
    CHECK(pl.assist.feedForward > 0.0f && pl.assist.feedForward <= hold * 0.95f + 1e-3f);
    CHECK(a.update(trainedNow + 2000, false, false, VentInfo{}, 39.9f, 40.0f, 0).assist.feedForward == 0);
    CHECK(a.update(trainedNow + 4000, true, true, VentInfo{}, 39.9f, 40.0f, 0).assist.feedForward == 0);
    CHECK(a.update(trainedNow + 6000, true, false, VentInfo{}, NAN, 40.0f, 0).assist.feedForward == 0);
    CHECK(a.update(trainedNow + 8000, true, false, VentInfo{}, 39.9f, INFINITY, 0).assist.feedForward == 0);
    a.setEnabled(false);
    CHECK(a.update(trainedNow + 10000, true, false, VentInfo{}, 39.9f, 40.0f, 0).assist.feedForward == 0); }
  // Authority is monotone in learning time (confidence): never more authority from less evidence.
  { float lastConf = -1, lastFF = -1;
    const double hours[] = {0.2, 0.7, 1.5, 3.0, 6.0};
    for (double h : hours) {
      AdaptiveV1 a; train(a, h, false);
      const float c = a.learner().profile().confidence;
      const Plan &pl = a.update(1000U + static_cast<uint32_t>(h * 3600000.0), true, false, VentInfo{}, 39.9f, 40.0f, 0);
      CHECK(c >= lastConf - 1.0f); CHECK(pl.assist.feedForward >= lastFF - 1.0f);
      if (c < 30.0f) CHECK(pl.assist.feedForward == 0.0f && !pl.hint.valid);   // low confidence: PID only
      lastConf = c; lastFF = pl.assist.feedForward;
    } }
  // Ventilation: bounded feed-forward, integral frozen with a bounded ceiling, recovery then idle.
  { AdaptiveV1 a; train(a, 8.0, true);
    CHECK(a.learner().profile().ventConfidence >= 50);
    VentInfo v; v.active = true;
    uint32_t t = trainedNow; float maxFF = 0;
    for (int i = 0; i < 120; ++i, t += 2000) {
      const Plan &pl = a.update(t, true, false, v, 40.0f, 40.0f, 5.0f);
      maxFF = std::max(maxFF, pl.ventFF);
      if (i > 3) CHECK(pl.assist.freezeIntegral && pl.assist.integralCeiling <= 5.0f + VentIntegralRisePct + 1e-3f);
    }
    CHECK(maxFF > 0.0f && maxFF <= VentCapPct + 1e-3f);
    // the compensation matches the learned effect: 100 * Kv / Kh, within the cap
    const float expect = std::min(VentCapPct, 100.0f * a.learner().profile().ventCoolingGain / a.learner().profile().heaterGain);
    CHECK(maxFF <= expect + 1e-3f);
    v.active = false;
    bool sawRecovery = false;
    for (int i = 0; i < 200; ++i, t += 2000) {
      const Plan &pl = a.update(t, true, false, v, 40.0f, 40.0f, 5.0f);
      if (pl.ventPhase == VentPhase::Recovery) sawRecovery = true;
    }
    CHECK(sawRecovery);
    CHECK(a.update(t, true, false, v, 40.0f, 40.0f, 5.0f).ventPhase == VentPhase::Idle);
    // A forced (safety) vent is never compensated.
    VentInfo f; f.forced = true; f.startsInSec = 5; float m = 0;
    for (int i = 0; i < 60; ++i, t += 2000) m = std::max(m, a.update(t, true, false, f, 40.0f, 40.0f, 0).ventFF);
    CHECK(m == 0.0f); }
  // Coordination disabled (ablation): no vent feed-forward.
  { AdaptiveV1 a; a.setVentCoordination(false); train(a, 8.0, true);
    VentInfo v; v.active = true; uint32_t t = trainedNow; float m = 0;
    for (int i = 0; i < 60; ++i, t += 2000) m = std::max(m, a.update(t, true, false, v, 40.0f, 40.0f, 0).ventFF);
    CHECK(m == 0.0f); }
  // Disable (maintenance / plant change) then re-enable: nothing learned before is trusted any more.
  { AdaptiveV1 a; train(a, 4.0, false);
    CHECK(a.update(trainedNow, true, false, VentInfo{}, 39.9f, 40.0f, 0).assist.feedForward > 0);
    a.setEnabled(false); a.setEnabled(true);
    const Plan &pl = a.update(trainedNow + 2000, true, false, VentInfo{}, 39.9f, 40.0f, 0);
    CHECK(pl.assist.feedForward == 0 && !pl.hint.valid && std::isinf(pl.assist.kiMax)); }
  // Overshoot guard: a real overshoot withdraws the profile (lock-out), whatever the confidence.
  { AdaptiveV1 a; train(a, 4.0, false);
    CHECK(a.update(trainedNow, true, false, VentInfo{}, 39.9f, 40.0f, 0).assist.feedForward > 0);
    const Plan &pl = a.update(trainedNow + 2000, true, false, VentInfo{}, 40.8f, 40.0f, 0);
    CHECK(pl.assist.feedForward == 0 && !pl.hint.valid && a.lockedOut(trainedNow + 2000));
    CHECK(a.update(trainedNow + 4000, true, false, VentInfo{}, 40.0f, 40.0f, 0).assist.feedForward == 0); }
}

static float step(ThermalController &c, uint32_t ms, float sp, float pv, const MachineConfig &cfg, bool en, const Assist *as) {
  return c.updateOnNewSample(ms, sp, pv, cfg, en, INFINITY, false, as);
}

static void pidAssistTests() {
  MachineConfig cfg; cfg.kp = 18; cfg.ki = 0.8f; cfg.kd = 0;
  // Default Assist == legacy exactly.
  { ThermalController a, b; Assist none; float pv = 30;
    for (uint32_t ms = 1000; ms <= 600000; ms += 2000) {
      const float x = a.updateOnNewSample(ms, 37.5f, pv, cfg, true);
      const float y = step(b, ms, 37.5f, pv, cfg, true, &none);
      CHECK(x == y);
      pv += 2 * (25 + 0.2f * x - pv) / 180;
    } }
  // Feed-forward is bumpless: stepping it moves the output by far less than the step.
  { ThermalController c; Assist as; as.feedForward = 10; float pv = 37.4f;
    for (uint32_t ms = 1000; ms <= 300000; ms += 2000) step(c, ms, 37.5f, pv, cfg, true, &as);
    const float before = c.output(); as.feedForward = 30;
    const float after = step(c, 302000, 37.5f, pv, cfg, true, &as);
    CHECK(std::fabs(after - before) < 5.0f); }
  // Bumpless config change while a vent boost is applied must not double-count the additive term.
  { ThermalController c; Assist as; as.feedForward = 10; as.addForward = 20; float pv = 37.0f;
    for (uint32_t ms = 1000; ms <= 300000; ms += 2000) c.updateOnNewSample(ms, 37.5f, pv, cfg, true, INFINITY, false, &as);
    const float before = c.output();
    MachineConfig changed = cfg; changed.kp = 25;
    c.applyConfigBumpless(300000, 37.5f, pv, changed);
    const float after = c.updateOnNewSample(302000, 37.5f, pv, changed, true, INFINITY, false, &as);
    CHECK(std::fabs(after - before) < 4.0f); }
  // NaN / out-of-range feed-forward cannot produce a non-finite or out-of-range output.
  { const float vals[] = {NAN, INFINITY, -50.0f, 1e9f};
    for (float f : vals) { ThermalController c; Assist as; as.feedForward = f; as.addForward = f;
      for (uint32_t ms = 1000; ms <= 100000; ms += 2000) {
        const float o = step(c, ms, 37.5f, 36.0f, cfg, true, &as);
        CHECK(std::isfinite(o) && o >= 0 && o <= 100);
      } } }
  // Anti-windup: the integral ceiling bounds the integral under sustained error (vent wind-up guard).
  { ThermalController c; Assist as; as.integralCeiling = 5.0f; float maxI = -1e9f;
    for (uint32_t ms = 1000; ms <= 600000; ms += 2000) {
      step(c, ms, 37.5f, 36.0f, cfg, true, &as); maxI = std::max(maxI, c.integral()); }
    CHECK(maxI <= 5.0f + 1e-3f); }
  // freezeIntegral: no growth while frozen with PV below SP.
  { ThermalController c; Assist as; as.freezeIntegral = true;
    for (uint32_t ms = 1000; ms <= 20000; ms += 2000) step(c, ms, 37.5f, 36.0f, cfg, true, &as);
    const float i0 = c.integral();
    for (uint32_t ms = 22000; ms <= 300000; ms += 2000) step(c, ms, 37.5f, 36.0f, cfg, true, &as);
    CHECK(c.integral() <= i0 + 1e-3f); }
  // kiMax only LOWERS the integral gain.
  { ThermalController lo, hi; Assist cap; cap.kiMax = 0.05f; Assist none;
    for (uint32_t ms = 1000; ms <= 200000; ms += 2000) {
      step(lo, ms, 37.5f, 37.0f, cfg, true, &cap);
      step(hi, ms, 37.5f, 37.0f, cfg, true, &none); }
    CHECK(lo.integral() < hi.integral());
    ThermalController up; Assist raise; raise.kiMax = 1000.0f;
    ThermalController ref;
    for (uint32_t ms = 1000; ms <= 200000; ms += 2000) {
      step(up, ms, 37.5f, 37.0f, cfg, true, &raise);
      step(ref, ms, 37.5f, 37.0f, cfg, true, &none); }
    CHECK(std::fabs(up.integral() - ref.integral()) < 1e-3f); }
  // Disabled / invalid inputs stay OFF whatever the assist says (safety precedence for the PID).
  { ThermalController c; Assist as; as.feedForward = 60; as.addForward = 20;
    for (uint32_t ms = 1000; ms <= 50000; ms += 2000) step(c, ms, 37.5f, 36.5f, cfg, true, &as);
    CHECK(step(c, 52000, 37.5f, 36.5f, cfg, false, &as) == 0);
    CHECK(step(c, 54000, 37.5f, NAN, cfg, true, &as) == 0);
    CHECK(step(c, 56000, NAN, 36.5f, cfg, true, &as) == 0); }
}

int main() {
  profileTests();
  learnerTests();
  plannerTests();
  pidAssistTests();
  std::printf("thermal-adaptive-unit: %d checks passed\n", checks);
  return 0;
}
