#pragma once
// Adaptive Thermal V1: online learning of the effective thermal profile.
//
// Pure C++ (no Arduino/FreeRTOS), fixed-size state, no heap, no String: the host
// simulation compiles exactly this code. Everything is learned from signals the real
// firmware has: the filtered temperature, the ACTUAL SSR state (integrated every control
// cycle), fan states, and fault/mode flags. Never from requested PID percent.
//
// Plant being identified (differential form, so ambient/loss are not needed):
//     dT/dt = Kh * u(t - D) - L(T) - Kv * vent(t)
// With non-overlapping windows A=[t-2W,t-W], B=[t-W,t]:
//     S_B - S_A = Kh * (u_B(D) - u_A(D)) + small,   S = window slope, u = delayed ACTUAL duty
// A bank of D candidates is fitted in parallel; the best fit gives the delay, its slope
// gives the gain. Heat-loss drift is second order inside 80 s and cancels in the difference.
#include "thermal_observer.h"
#include "thermal_profile.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace MayapThermal {

namespace Policy {
constexpr uint32_t WindowMs = 80000;             // W: slope window (>= 2.5x a typical 30 s heater lag)
constexpr uint32_t SlopeWindowMs = 90000;        // robust slope for the startup controller
constexpr uint32_t SettleMs = 60000;             // clean time required after any disturbance
constexpr uint32_t VentRecoveryMs = 150000;      // heater learning stays locked after vent OFF
constexpr uint32_t MaxSampleGapMs = 10000;       // larger gap: no continuity
constexpr uint32_t VentSettleMs = 25000;         // vent effect needs this long to appear
constexpr uint32_t VentMinOnMs = VentSettleMs + WindowMs + 5000;  // shorter events are unusable
constexpr float JumpMinC = 0.5f;                 // per-sample jump gate floor
constexpr float DoorRawJumpC = 0.5f;
constexpr float HighMarginC = 0.3f;
constexpr float MinDelayScore = 0.25f;           // delay fit quality required before it is published
constexpr float ParsimonyFraction = 0.92f;       // near-equal fit threshold for the smallest delay
constexpr float RegulationBandC = 1.0f;          // inside this band around SP the duty is the feedback law (closed-loop bias)
#ifndef MAYAP_THERMAL_REGULATION_WEIGHT
#define MAYAP_THERMAL_REGULATION_WEIGHT 0.5f
#endif
constexpr float RegulationWeight = MAYAP_THERMAL_REGULATION_WEIGHT;  // ... so such windows count only this much
#ifndef MAYAP_THERMAL_MIN_DUTY_STEP
#define MAYAP_THERMAL_MIN_DUTY_STEP 0.15f
#endif
constexpr float MinDutyStep = MAYAP_THERMAL_MIN_DUTY_STEP;  // smallest duty step worth learning from
constexpr float SxxMin = 1.0f;                   // information needed before trusting a gain
constexpr float SxxTarget = 8.0f;                // information for full gain score
constexpr float ForgetSlow = 0.9985f;            // per update (~2 s): ~22 min memory
constexpr float ForgetFast = 0.994f;             // ~5.5 min memory (change detection)
constexpr float FastSxxMin = 1.0f;
constexpr float MismatchRatio = 1.349859f;        // exp(0.30): Kfast/Kslow (or its inverse) beyond this is a plant change
constexpr float RecoverRatio = 1.161834f;         // exp(0.15): back inside this the gain evidence has recovered (no libm log)
constexpr uint16_t MismatchRuns = 24;            // consecutive informative evaluations
constexpr uint16_t RecoverRuns = 40;
constexpr uint32_t FastFreshMs = 600000UL;         // Smart: the fast gain counts as change evidence only if a full-weight window refreshed it this recently
constexpr uint32_t MinValidLearningSec = 300;
constexpr uint32_t HoldWindowMs = 120000;         // average ACTUAL duty over a flat window (minimum)
constexpr uint32_t HoldWindowMaxMs = 300000;      //   ... stretched to 3x the learned delay, up to this
constexpr float HoldBandBelowC = 6.0f;            // learn while PV is within [SP-6.0, SP+0.4]; an exact-SP
constexpr float HoldBandAboveC = 0.40f;           // band would deadlock a brake that lacks hold power
constexpr float HoldRangeC = 0.25f;               // max-min PV inside the window
constexpr float HoldDriftC = 0.12f;               // |PV end - PV start|
constexpr float HoldMaxStepPct = 8.0f;            // first windows (profile still forming)
constexpr uint32_t HoldFormingWindows = 3;        // wide band + big steps only until this many windows
constexpr float HoldSteadyBandC = 0.6f;           // afterwards only windows right at the setpoint count
constexpr float HoldMaxRisePct = 1.0f;            // a LOWER hold than truth is safe, a HIGHER one overshoots
constexpr float HoldFastRisePct = 3.0f;           // Smart: per-window rise when two flat windows agree
constexpr float HoldFastFraction = 0.92f;         //   ... never past 92 % of the lower measurement
constexpr float HoldFastAgreePct = 2.0f;          //   ... "agree" = within max(2 pp, 10 %)
constexpr float HoldMaxFallPct = 4.0f;            //   after a disturbance ends: rise slowly, fall faster
constexpr float HoldMismatchAbsPct = 5.0f;        // plant-change evidence: observed hold differs from the profile by
constexpr float HoldMismatchRel = 0.25f;          //   max(5 pp, 25 %) ...
constexpr float HoldMismatchSigmas = 2.5f;        //   ... and 2.5 sigma of the window-to-window scatter
constexpr float SensorLsbC = 0.1f;                //   (+ one sensor LSB of window drift, as duty)
constexpr uint32_t MismatchMaxMs = 5400000UL;      // a mismatch expires after 90 min if nothing clears it
constexpr uint16_t HoldMismatchRuns = 3;          //   ... for this many consecutive windows (same sign)
constexpr uint32_t MismatchRefractoryMs = 1200000;  // 20 min between two plant-change declarations
constexpr uint16_t HoldAgreeRuns = 4;             // agreement for this many windows ends a mismatch
constexpr float RelearnDutyStep = 0.06f;          // smaller informative step accepted while re-learning
}  // namespace Policy

enum class GateReason : uint8_t {
  Open, Disabled, Sensor, Safety, HeaterBlocked, Tune, Test, Maintenance, Recovery,
  Vent, Fan, Gap, Jump, Door, Settling
};
inline const char *gateReasonName(GateReason r) {
  static const char *const n[] = {"OPEN", "DISABLED", "SENSOR", "SAFETY", "HEATER_BLOCKED", "TUNE",
                                  "TEST", "MAINTENANCE", "RECOVERY", "VENT", "FAN", "GAP", "JUMP",
                                  "DOOR", "SETTLING"};
  const uint8_t i = static_cast<uint8_t>(r);
  return i < 15 ? n[i] : "?";
}

struct LearnInput {
  float pv = NAN;       // filtered CONTROL temperature
  float raw = NAN;      // safety temperature (never below the raw probe)
  float sp = NAN;
  float high = NAN;     // High alarm threshold
  bool sensor = false;
  bool fanStable = false;      // circulation fan in normal state
  bool ventActive = false;     // exhaust fan ACTUALLY on
  bool safety = false, recovery = false, test = false, maintenance = false, tune = false;
  bool heaterBlocked = false;  // SSR inhibited / master not picked up / actuator not ready
};

struct Hints {  // slow estimates produced by the existing ThermalObserver (reused, not duplicated)
  float hold = 0, coast = 0, coastSec = 0;
  uint32_t holdWindows = 0, coastWindows = 0;
  bool valid = false;
};

// Delay candidates (s). Namespace scope: no ODR/definition issue in C++11.
constexpr float DelayGrid[10] = {0, 8, 16, 25, 35, 50, 70, 95, 125, 160};

class ThermalLearner {
 public:
  static constexpr uint8_t Cands = 10;
  #ifndef MAYAP_THERMAL_PV_SCALE
#define MAYAP_THERMAL_PV_SCALE 200.0f   // history PV in 0.005 C units (uint16)
#endif
#ifndef MAYAP_THERMAL_RING
#define MAYAP_THERMAL_RING 176   // 352 s at the 2 s cadence: 2W (160 s) + the longest delay candidate (160 s)
#endif
  static constexpr uint8_t RingSize = MAYAP_THERMAL_RING;

  ThermalLearner() { reset(); }

  void reset() {
    const bool smart = smartHold_;     // a configuration, not learned state
    *this = ThermalLearner(Tag{});
    smartHold_ = smart;
  }
  // Smart Thermal hold convergence (flag, default MAYAP_SMART_THERMAL): two agreeing flat windows may move the hold faster
  // (never above 92 % of the lower measurement), and a hold that is still catching up with the measured equilibrium is
  // not reported as a plant change.
  void setSmartHold(bool on) { smartHold_ = on; }
  bool smartHold() const { return smartHold_; }
  // The stored profile is a seed, never evidence: confidence is capped by the caller and
  // the accumulators start with only a small pseudo-observation weight.
  // `measuredHold`: the hold in `p` was MEASURED just now by an accepted Smart AutoTune (relay mean duty at the setpoint),
  // so the wide forming band that would overwrite it with a biased-low off-setpoint window is skipped.
  void seed(const ThermalProfile &p, uint8_t confidenceCap, bool measuredHold = false) {
    if (!profileRangesValid(p)) return;
    profile_ = p;
    sanitizeProfile(profile_);
    profile_.confidence = std::min<uint8_t>(profile_.confidence, confidenceCap);
    profile_.ventConfidence = std::min<uint8_t>(profile_.ventConfidence, confidenceCap);
    int best = 0;
    float bd = 1e9f;
    for (uint8_t i = 0; i < Cands; ++i) {
      const float d = std::fabs(DelayGrid[i] - profile_.heaterDelaySec);
      if (d < bd) { bd = d; best = i; }
    }
    best_ = static_cast<uint8_t>(best);
    const float pseudo = 1.5f;
    for (uint8_t i = 0; i < Cands; ++i) { sxxS_[i] = sxxF_[i] = 0; sxyS_[i] = sxyF_[i] = 0; }
    sxxS_[best_] = pseudo; sxyS_[best_] = pseudo * profile_.heaterGain;
    sxxF_[best_] = pseudo * 0.5f; sxyF_[best_] = pseudo * 0.5f * profile_.heaterGain;
    syyS_ = pseudo * profile_.heaterGain * profile_.heaterGain * 1.2f;
    seeded_ = true;
    holdWindows_ = measuredHold ? Policy::HoldFormingWindows : 0U;  // a stored hold is a seed (no authority until re-measured); a freshly measured one is not
    // A stored vent estimate is a seed: no compensation until a live vent event has requalified it.
    profile_.ventConfidence = 0;
    ventEvents_ = 0;
    updateState();
  }
  void setEnabled(bool on) {
    if (!on && enabled_) { clearWindows(); }
    enabled_ = on;
  }
  bool enabled() const { return enabled_; }

  // Every control cycle. Integrates the REAL actuator state.
  void tick(uint32_t now, bool actualOn) {
    if (!tickSeen_) { tickSeen_ = true; tickAt_ = now; return; }
    const uint32_t dt = now - tickAt_;
    tickAt_ = now;
    if (dt > 1000U) { gapSeen_ = true; return; }  // stall: unknown actuation earns nothing
    if (actualOn) cumOnMs_ += dt;
  }

  // Every NEW filtered sensor sample.
  void sample(uint32_t now, const LearnInput &in, const Hints &hint) {
    hint_ = hint;
    const uint32_t dt = sampleSeen_ ? now - sampleAt_ : 0U;
    sampleAt_ = now;
    const bool first = !sampleSeen_;
    sampleSeen_ = true;
    if (!enabled_) { gate_ = GateReason::Disabled; return; }
    const bool finite = std::isfinite(in.pv) && std::isfinite(in.sp);
    if (!finite || !in.sensor) { invalidate(now, GateReason::Sensor, true); ventCancel(); return; }

    // Ring always records the filtered sample (even when unclean): windows are time based.
    const float prevPv = lastPv_, prevRaw = lastRaw_;
    const bool hadPrev = !first && std::isfinite(prevPv);
    lastPv_ = in.pv; lastRaw_ = in.raw;
    pushRing(now, in.pv);
    {
      float p0; Cum c;
      float p1 = in.pv;
      slopeValid_ = lookup(now - Policy::SlopeWindowMs, p0, c);
      if (slopeValid_) slope90_ = (p1 - p0) / (Policy::SlopeWindowMs * 0.001f);
    }

    GateReason g = evaluate(now, in, dt, hadPrev ? prevPv : NAN, prevRaw);
    // Vent estimator has its own (looser) gate: the vent itself is the experiment.
    trackVent(now, in, g, dt);
    if (in.ventActive) { ventOn_ = true; } else if (ventOn_) { ventOn_ = false; ventOffAt_ = now; }
    if (g != GateReason::Open) {
      gate_ = g;
      if (g != GateReason::Settling) invalidate(now, g, false);
      return;
    }
    // Gate open. The differential needs 2W of clean history.
    if (cleanSince_ == 0U) { cleanSince_ = now; }
    gate_ = (now - cleanSince_) >= 2U * Policy::WindowMs ? GateReason::Open : GateReason::Settling;
    if (gate_ != GateReason::Open) { updateConfidence(); return; }
    { const uint32_t add = std::min<uint32_t>(dt, Policy::MaxSampleGapMs);  // saturate: no 49.7-day wrap
      validMs_ = validMs_ > 0xffffffffU - add ? 0xffffffffU : validMs_ + add; }
    profile_.validLearningSec = validMs_ / 1000U;
    updateRegression(now);
    trackHold(now, in);
    updateDerived();
    updateConfidence();
  }

  const ThermalProfile &profile() const { return profile_; }
  bool slopeValid() const { return slopeValid_ && gate_ != GateReason::Sensor && gate_ != GateReason::Gap; }
  float slope90() const { return slope90_; }
  LearnState state() const { return static_cast<LearnState>(profile_.state); }
  GateReason gate() const { return gate_; }
  float predictionError() const { return pe_; }
  float gainScore() const { return gainScore_; }
  float delayScore() const { return delayScore_; }
  float infoSlow() const { return sxxS_[best_]; }
  bool mismatch() const { return mismatch_; }
  uint32_t ventEvents() const { return ventEvents_; }
  uint32_t mismatchEvents() const { return mismatchEvents_; }
  uint32_t mismatchBy(uint8_t reason) const { return reason < 4 ? mismatchByReason_[reason] : 0U; }
  uint32_t holdWindows() const { return holdWindows_; }
  uint32_t outliers() const { return outliers_; }
  uint32_t updates() const { return updates_; }
  float fastGain() const { return gainFast_; }
  uint8_t bestIndex() const { return best_; }
  float jumpLimit(uint32_t dtMs) const {
    return std::max(Policy::JumpMinC, 2.5f * profile_.heaterGain * dtMs * 0.001f);
  }
  // Measured (observer) coast over the model coast, floored at 1: used ONLY to make the brake
  // more cautious when reality coasts further than Kh*delay predicts.
  float coastScale() const { return coastScale_; }
  // Component trust used by the control side (independent of the overall number).
  float gainTrust() const { return gainScore_ * fitScore(); }
  float delayTrust() const { return delayScore_; }
  float holdTrust() const { return holdScore_; }
  float ventTrust() const { return profile_.ventConfidence * 0.01f; }
  // Take over a degraded decision from outside (e.g. configuration changed).
  void degrade() {
    profile_.confidence = static_cast<uint8_t>(profile_.confidence / 2);
    profile_.ventConfidence = static_cast<uint8_t>(profile_.ventConfidence / 2);
    clearWindows();
    updateState();
  }
  void suspendCritical() {
    profile_.confidence = 0; profile_.ventConfidence = 0;
    clearWindows();
    updateState();
  }

 private:
  struct Tag {};
  explicit ThermalLearner(Tag) {
    profile_ = defaultProfile();
    for (uint8_t i = 0; i < Cands; ++i) { sxxS_[i] = sxyS_[i] = sxxF_[i] = sxyF_[i] = 0; }
    syyS_ = 0;
  }
  // Compact history (6 B/sample, no dynamic allocation): the newest sample is kept absolute
  // (time, cumulative actual ON-ms); every entry stores only the interval to the sample before it
  // and the PV in 0.005 C units. Older samples are reconstructed by walking back from the newest.
  struct Entry { uint16_t dt; uint16_t pv; uint16_t on; };
  static constexpr float PvScale = MAYAP_THERMAL_PV_SCALE;
  static uint16_t encodePv(float pv) {
    const float v = pv * PvScale + 0.5f;
    return v <= 0.0f ? 0U : v >= 65535.0f ? 65535U : static_cast<uint16_t>(v);
  }

  // ---- ring / window arithmetic -------------------------------------------------------
  void pushRing(uint32_t now, float pv) {
    Entry e;
    e.pv = encodePv(pv);
    if (count_ == 0U) { e.dt = 0U; e.on = 0U; }
    else {
      const uint32_t dt = now - newestT_;
      if (dt > 65535U) { count_ = 0U; head_ = 0U; e.dt = 0U; e.on = 0U; }   // a long outage is a new history
      else {
        const uint32_t on = std::min<uint32_t>(cumOnMs_ - newestCum_, dt);
        e.dt = static_cast<uint16_t>(dt); e.on = static_cast<uint16_t>(on);
      }
    }
    ring_[head_] = e;
    head_ = static_cast<uint8_t>((head_ + 1U) % RingSize);
    if (count_ < RingSize) ++count_;
    newestT_ = now; newestCum_ = cumOnMs_;
  }
  // Interpolated pv and cumulative ON-ms at an absolute time; false when outside the ring.
  // Cumulative ON-time in 1/256 ms (int64: exact, no soft-double arithmetic on the target).
  typedef int64_t Cum;
  bool lookup(uint32_t target, float &pv, Cum &cum) const {
    if (count_ < 2) return false;
    if (static_cast<int32_t>(target - newestT_) > 0) return false;
    uint32_t tHi = newestT_, cHi = newestCum_;
    uint8_t idx = static_cast<uint8_t>((head_ + RingSize - 1U) % RingSize);
    float pvHi = ring_[idx].pv * (1.0f / PvScale);
    for (uint8_t b = 0; b + 1U < count_; ++b) {
      const Entry &hiE = ring_[idx];
      const uint32_t tLo = tHi - hiE.dt, cLo = cHi - hiE.on;
      idx = static_cast<uint8_t>((idx + RingSize - 1U) % RingSize);
      const float pvLo = ring_[idx].pv * (1.0f / PvScale);
      if (static_cast<int32_t>(target - tLo) >= 0 && static_cast<int32_t>(tHi - target) >= 0) {
        const uint32_t span = tHi - tLo;
        const float f = span ? static_cast<float>(target - tLo) / static_cast<float>(span) : 1.0f;
        pv = static_cast<float>(pvLo + (pvHi - pvLo) * f);
        cum = (static_cast<Cum>(cLo) << 8) +
              static_cast<Cum>(static_cast<float>(static_cast<int32_t>(cHi - cLo)) * 256.0f * f);
        return true;
      }
      tHi = tLo; cHi = cLo; pvHi = pvLo;
    }
    return false;
  }
  // cumulative on-time is a uint32 millisecond counter: a difference across its wrap is still small
  static float cumDiff(Cum hi, Cum lo) {   // ms
    Cum d = hi - lo;
    if (d < -(static_cast<Cum>(1) << 39)) d += static_cast<Cum>(1) << 40;
    return static_cast<float>(d) * (1.0f / 256.0f);
  }
  // mean ACTUAL duty (0..1) over [a,b]
  bool meanDuty(uint32_t a, uint32_t b, float &duty) const {
    float p; Cum ca, cb;
    if (!lookup(a, p, ca) || !lookup(b, p, cb)) return false;
    const float span = static_cast<float>(b - a);
    if (span <= 0) return false;
    duty = std::min(1.0f, std::max(0.0f, cumDiff(cb, ca) / span));
    return true;
  }
  bool slopeAt(uint32_t te, float &slope) const {
    float p0, p1; Cum c;
    if (!lookup(te - Policy::WindowMs, p0, c) || !lookup(te, p1, c)) return false;
    slope = (p1 - p0) / (Policy::WindowMs * 0.001f);
    return true;
  }

  // ---- gate ------------------------------------------------------------------------------
  GateReason evaluate(uint32_t now, const LearnInput &in, uint32_t dt, float prevPv, float prevRaw) {
    if (!in.sensor) return GateReason::Sensor;
    if (in.safety || (std::isfinite(in.high) &&
        (in.pv >= in.high - Policy::HighMarginC ||
         (std::isfinite(in.raw) && in.raw >= in.high - Policy::HighMarginC)))) return GateReason::Safety;
    if (in.heaterBlocked) return GateReason::HeaterBlocked;
    if (in.tune) return GateReason::Tune;
    if (in.test) return GateReason::Test;
    if (in.maintenance) return GateReason::Maintenance;
    if (in.recovery) return GateReason::Recovery;
    if (in.ventActive || (ventOffAt_ != 0U && now - ventOffAt_ < Policy::VentRecoveryMs)) return GateReason::Vent;
    if (!in.fanStable) return GateReason::Fan;
    if (gapSeen_ || dt > Policy::MaxSampleGapMs) { gapSeen_ = false; return GateReason::Gap; }
    if (std::isfinite(prevPv) && std::fabs(in.pv - prevPv) > jumpLimit(dt ? dt : 2000U)) return GateReason::Jump;
    if (std::isfinite(in.raw) && std::isfinite(prevRaw) && std::fabs(in.raw - prevRaw) > Policy::DoorRawJumpC)
      return GateReason::Door;
    if (std::isfinite(lastSp_) && std::fabs(in.sp - lastSp_) > 0.01f) {
      // A setpoint edit is a one-shot disturbance: drop the windows that span both operating points.
      lastSp_ = in.sp; invalidate(now, GateReason::Settling, false);
      // Delay candidates look back up to DelayGrid[max] before the window: keep the gate shut until
      // no candidate can pair actuator history from before the edit with post-edit temperature.
      settleUntil_ = now + Policy::SettleMs + static_cast<uint32_t>(DelayGrid[Cands - 1] * 1000.0f);
      // Equilibrium duty depends on the setpoint: hold learned at the old target is no longer evidence.
      holdWindows_ = 0U; holdScore_ = 0.0f; holdPrevFlat_ = false; holdConverging_ = false; holdSettled_ = false; holdAgreeStreak_ = 0;
      return GateReason::Settling;
    }
    lastSp_ = in.sp;
    if (settleUntil_ != 0U && static_cast<int32_t>(now - settleUntil_) < 0) return GateReason::Settling;
    return GateReason::Open;
  }
  void invalidate(uint32_t now, GateReason why, bool critical) {
    hold_.active = false;
    cleanSince_ = 0U;
    settleUntil_ = now + Policy::SettleMs;
    gate_ = why;
    if (critical) {  // sensor lost/NaN: the confidence is not trustworthy any more
      profile_.confidence = std::min<uint8_t>(profile_.confidence, 20);
    }
    updateState();
  }
  void clearWindows() {
    cleanSince_ = 0U; settleUntil_ = 0U; count_ = 0; head_ = 0; hold_.active = false; ventCancel();
  }

  // ---- regression ------------------------------------------------------------------------
  void updateRegression(uint32_t te) {
    float sB, sA;
    if (!slopeAt(te, sB) || !slopeAt(te - Policy::WindowMs, sA)) return;
    const float dS = sB - sA;
    float du[Cands];
    for (uint8_t i = 0; i < Cands; ++i) {
      const uint32_t d = static_cast<uint32_t>(DelayGrid[i] * 1000.0f);
      float uB, uA;
      if (!meanDuty(te - d - Policy::WindowMs, te - d, uB) ||
          !meanDuty(te - d - 2U * Policy::WindowMs, te - d - Policy::WindowMs, uA)) return;
      du[i] = uB - uA;
    }
    // Innovation (prediction residual of the CURRENT model) -> prediction error + outlier gate.
    const float khNow = profile_.heaterGain;
    const float r = dS - khNow * du[best_];
    const float scale = std::max(0.25f * khNow, 0.0005f);
    const float sigma = std::sqrt(std::max(rVar_, 1e-10f));
    if (updates_ > 20 && std::fabs(r) > 6.0f * sigma && outlierRun_ < 8) {
      ++outliers_; ++outlierRun_;     // isolated spike: never learned from
      return;
    }
    outlierRun_ = 0;
    rVar_ = updates_ ? 0.99f * rVar_ + 0.01f * r * r : r * r;
    const float norm = std::fabs(r) / scale;
    pe_ = updates_ ? 0.985f * pe_ + 0.015f * std::min(norm, 8.0f) : std::min(norm, 8.0f);
    ++updates_;

    // Closed-loop guard: tiny duty differences carry no information about the plant but
    // are mostly the feedback law talking to itself, which biases the gain. Only windows
    // where SOME candidate sees a real duty step are accumulated.
    float duMax = 0;
    for (uint8_t i = 0; i < Cands; ++i) duMax = std::max(duMax, std::fabs(du[i]));
    const bool relearning = mismatch_ || wasDegraded_;
    const float minStep = relearning ? Policy::RelearnDutyStep : Policy::MinDutyStep;
    if (duMax < minStep) return;
    // Closed-loop guard #2: while PV is regulated at the setpoint the duty is the feedback law
    // answering PV noise, which biases the gain (measured +47 % on a heavy plant). The gain is
    // learned while PV is still far from SP (heat-up, approach, disturbance recovery), which is
    // close to an open-loop experiment. Re-learning after a detected change lifts this guard.
    float weight = 1.0f;
    if (!relearning && std::isfinite(lastSp_) && std::fabs(lastPv_ - lastSp_) <= Policy::RegulationBandC) {
      weight = Policy::RegulationWeight;  // still informative, but at a fraction of the open-loop-like weight
    }
    const float lamS = Policy::ForgetSlow, lamF = Policy::ForgetFast;
    syyS_ = lamS * syyS_ + weight * dS * dS;
    for (uint8_t i = 0; i < Cands; ++i) {
      sxxS_[i] = lamS * sxxS_[i] + weight * du[i] * du[i];
      sxyS_[i] = lamS * sxyS_[i] + weight * du[i] * dS;
      // The fast (change-detection) accumulators take only full-weight windows: regulation-band
      // windows carry the closed-loop bias and would fake a "gain change".
      if (weight >= 1.0f) {
        sxxF_[i] = lamF * sxxF_[i] + du[i] * du[i];
        sxyF_[i] = lamF * sxyF_[i] + du[i] * dS;
        fastUpdateAt_ = sampleAt_ ? sampleAt_ : 1U;
      }
    }
    // best delay = max explained variance sxy^2/sxx among informed candidates
    int best = best_;
    float bestScore = -1;
    for (uint8_t i = 0; i < Cands; ++i) {
      if (sxxS_[i] < Policy::SxxMin) continue;
      const float s = sxyS_[i] * sxyS_[i] / (sxxS_[i] + 1e-6f);
      if (s > bestScore) { bestScore = s; best = i; }
    }
    if (bestScore >= 0) {
      // Parsimony: a periodic excitation (a limit cycle) fits delay D and D+k*period equally
      // well. Among near-equal fits take the SMALLEST delay: a transport delay is never longer
      // than it must be to explain the data.
      // (only with real evidence: with a couple of windows every candidate "fits" and the
      // smallest delay would win by default)
      if (sxxS_[best] >= 2.0f * Policy::SxxMin) {
        for (uint8_t i = 0; i < Cands; ++i) {
          if (sxxS_[i] < Policy::SxxMin) continue;
          const float sc = sxyS_[i] * sxyS_[i] / (sxxS_[i] + 1e-6f);
          if (sc >= Policy::ParsimonyFraction * bestScore) { if (static_cast<int>(i) < best) best = i; break; }
        }
      }
      // hysteresis: switch only for a clear (>8 %) improvement
      const float cur = sxxS_[best_] >= Policy::SxxMin ? sxyS_[best_] * sxyS_[best_] / (sxxS_[best_] + 1e-6f) : -1;
      const float candScore = sxxS_[best] >= Policy::SxxMin ? sxyS_[best] * sxyS_[best] / (sxxS_[best] + 1e-6f) : -1;
      if (best != best_ && candScore > cur * 1.08f) best_ = static_cast<uint8_t>(best);
      else if (best != best_ && best < best_ && candScore >= Policy::ParsimonyFraction * cur) best_ = static_cast<uint8_t>(best);
      // delay score: explained fraction and margin to the non-adjacent runner-up
      const float explained = syyS_ > 1e-12f ? std::min(1.0f, bestScore / syyS_) : 0.0f;
      float runner = 0;
      for (uint8_t i = 0; i < Cands; ++i) {
        if (i + 1U >= best_ && i <= best_ + 1U) continue;
        if (sxxS_[i] < Policy::SxxMin) continue;
        runner = std::max(runner, sxyS_[i] * sxyS_[i] / (sxxS_[i] + 1e-6f));
      }
      const float margin = bestScore > 0 ? 1.0f - std::min(1.0f, runner / bestScore) : 0.0f;
      delayScore_ = std::min(1.0f, explained * 1.6f) * std::min(1.0f, 0.4f + margin);
    }
    // fast gain for change detection (same delay)
    gainFast_ = sxxF_[best_] >= Policy::FastSxxMin ? sxyF_[best_] / (sxxF_[best_] + 0.05f) : 0.0f;
    detectMismatch();
  }
  void detectMismatch() {
    // Safety valve: a mismatch whose own detector never gets the evidence to clear it still expires
    // (and is simply raised again if the evidence persists).
    if (mismatch_ && lastMismatchAt_ != 0U && sampleAt_ - lastMismatchAt_ > Policy::MismatchMaxMs) mismatch_ = false;
    const float slow = sxxS_[best_] >= Policy::SxxMin ? sxyS_[best_] / (sxxS_[best_] + 0.05f) : 0.0f;
    // Smart: the fast accumulators take only full-weight windows (PV away from the set point). While PV is regulated at SP they
    // are not refreshed, but the slow ones keep taking the closed-loop-biased reduced-weight windows and drift upward, so
    // "stale fast" against "drifting slow" is not evidence of a plant change (it made the assist drop for ~30 min in AutoTune
    // case full165). A change that matters disturbs PV and refreshes the fast accumulators; the hold detector covers equilibrium shifts.
    const bool staleFast = smartHold_ && (fastUpdateAt_ == 0U || sampleAt_ - fastUpdateAt_ > Policy::FastFreshMs);
    if (slow <= 0 || gainFast_ <= 0 || sxxF_[best_] < Policy::FastSxxMin || staleFast) {
      if (mismatchRun_ > 0) --mismatchRun_;            // no evidence: decay slowly
      if (recoverRun_ > 0) --recoverRun_;
      return;
    }
    const float ratio = gainFast_ > slow ? gainFast_ / slow : slow / gainFast_;   // >= 1
    if (ratio > Policy::MismatchRatio) { if (mismatchRun_ < 65000) ++mismatchRun_; recoverRun_ = 0; }
    else {
      if (mismatchRun_ > 2) mismatchRun_ = static_cast<uint16_t>(mismatchRun_ - 2); else mismatchRun_ = 0;
      if (ratio < Policy::RecoverRatio) { if (recoverRun_ < 65000) ++recoverRun_; }
    }
    if (!mismatch_ && mismatchRun_ >= Policy::MismatchRuns) {
      declareMismatch(1);
    } else if (mismatch_ && mismatchCause_ != 2 && recoverRun_ >= Policy::RecoverRuns) {
      mismatch_ = false; mismatchRun_ = 0;   // only the detector that raised it may clear it
    }
  }
  // Hold-based plant-change detector (heater power, losses, ambient: anything that moves the
  // equilibrium duty). The reference is FIXED when a deviating run starts, so the profile following
  // the change cannot make the evidence disappear; the threshold also grows with the observed
  // window-to-window scatter, so a loop that limit-cycles does not cry wolf.
  void evaluateHoldWindow(float obs, float profileHold, float quantPp) {
    const float ref = holdMismatchRun_ == 0 ? profileHold : holdRef_;
    if (holdObsSeen_ && holdMismatchRun_ == 0) {
      const float c = std::min(20.0f, std::max(-20.0f, obs - lastHoldObs_));
      holdDevVar_ = holdDevSeen_ ? 0.8f * holdDevVar_ + 0.2f * 0.5f * c * c : 0.5f * c * c;
      holdDevSeen_ = true;
    }
    lastHoldObs_ = obs; holdObsSeen_ = true;
    const float sigma = std::sqrt(std::max(0.0f, holdDevVar_));
    // A 'flat' window still hides up to one sensor LSB of drift; on a heavy, low-gain plant that is
    // many percentage points of duty, so the threshold carries that quantisation floor.
    const float thr = std::max(std::max(Policy::HoldMismatchAbsPct, Policy::HoldMismatchRel * std::max(ref, 1.0f)) + quantPp,
                               Policy::HoldMismatchSigmas * sigma);
    const float dev = obs - ref;
    const int sign = dev > 0 ? 1 : -1;
    if (std::fabs(dev) > thr) {
      holdAgreeRun_ = 0;
      if (holdMismatchRun_ > 0 && sign == holdDevSign_) { if (holdMismatchRun_ < 65000) ++holdMismatchRun_; }
      else { holdMismatchRun_ = 1; holdDevSign_ = sign; holdRef_ = ref; }
      if (!mismatch_ && holdMismatchRun_ >= Policy::HoldMismatchRuns) declareMismatch(2);
    } else {
      holdMismatchRun_ = 0;
      if (std::fabs(obs - profile_.holdPowerPct) < 0.5f * thr && holdAgreeRun_ < 65000) ++holdAgreeRun_;
      if (mismatch_ && mismatchCause_ == 2 && holdAgreeRun_ >= Policy::HoldAgreeRuns) { mismatch_ = false; mismatchRun_ = 0; recoverRun_ = 0; }
    }
  }
  // The plant the profile describes is no longer the plant being controlled.
  void declareMismatch(uint8_t reason = 0) {
    if (reason < 4) ++mismatchByReason_[reason];
    // Refractory period: a plant is not "changed" again minutes after the last declaration.
    if (lastMismatchAt_ != 0U && sampleAt_ - lastMismatchAt_ < Policy::MismatchRefractoryMs) return;
    lastMismatchAt_ = sampleAt_ ? sampleAt_ : 1U;
    mismatch_ = true; recoverRun_ = 0; holdAgreeRun_ = 0; ++mismatchEvents_;
    holdSettled_ = false; holdAgreeStreak_ = 0;   // re-learning: the hold is "catching up" again
    mismatchCause_ = reason == 2 ? 2 : 1;
    // Make the slow accumulators forget the old plant quickly.
    for (uint8_t i = 0; i < Cands; ++i) { sxxS_[i] *= 0.35f; sxyS_[i] *= 0.35f; }
    syyS_ *= 0.35f;
    profile_.confidence = static_cast<uint8_t>(profile_.confidence * 0.5f);
    pe_ = std::max(pe_, 1.5f);
    updateState();
  }

  // ---- hold power: duty that exactly balances the heat loss near the setpoint ---------------
  // Energy balance over a 120 s clean window (linear plant, so averages are exact):
  //     loss_duty = mean ACTUAL duty (delayed by D) - dPV / (Kh * T)
  // valid at any slope, so the estimate does not wait for PV to sit exactly on SP (which a
  // brake that is starved of hold power would never allow). A flat window needs no Kh at all.
  void trackHold(uint32_t now, const LearnInput &in) {
    const float below = holdWindows_ < Policy::HoldFormingWindows ? Policy::HoldBandBelowC : Policy::HoldSteadyBandC;
    if (in.pv < in.sp - below || in.pv > in.sp + Policy::HoldBandAboveC) { hold_.active = false; return; }
    if (!hold_.active) {
      // The window (<= 300 s) plus the delay (<= 160 s) can exceed what the ring remembers, so the
      // delayed cumulative duty at the START is captured now, while the ring still reaches it.
      float pvTmp; Cum cumStart;
      const uint32_t d0 = static_cast<uint32_t>(profile_.heaterDelaySec * 1000.0f);
      if (!lookup(now - d0, pvTmp, cumStart)) return;
      hold_ = HoldWindow{};
      hold_.active = true; hold_.t0 = now; hold_.pv0 = in.pv; hold_.lo = hold_.hi = in.pv;
      hold_.cumDelayed0 = cumStart; hold_.delayMs = d0;
      return;
    }
    hold_.lo = std::min(hold_.lo, in.pv);
    hold_.hi = std::max(hold_.hi, in.pv);
    // The averaging window must cover whole control cycles of a delayed plant: 3x the learned delay.
    const uint32_t windowMs = std::min<uint32_t>(Policy::HoldWindowMaxMs,
        std::max<uint32_t>(Policy::HoldWindowMs, static_cast<uint32_t>(3.0f * profile_.heaterDelaySec * 1000.0f)));
    if (now - hold_.t0 < windowMs) return;
    const float spanS = (now - hold_.t0) * 0.001f;
    const float dPv = in.pv - hold_.pv0;
    const bool flat = hold_.hi - hold_.lo <= Policy::HoldRangeC && std::fabs(dPv) <= Policy::HoldDriftC;
    float ud = 0;
    bool haveDuty = false;
    {
      float pvTmp; Cum cumEnd;
      if (lookup(now - hold_.delayMs, pvTmp, cumEnd)) {
        ud = std::min(1.0f, std::max(0.0f, cumDiff(cumEnd, hold_.cumDelayed0) / static_cast<float>(now - hold_.t0)));
        haveDuty = true;
      }
    }
    float obs = NAN;
    if (haveDuty) {
      if (flat) obs = 100.0f * ud;  // equilibrium: duty == loss duty
      else if (gainTrust() >= 0.5f && profile_.heaterGain > Limits::GainMin)
        obs = 100.0f * (ud - dPv / (profile_.heaterGain * spanS));
    }
    if (std::isfinite(obs)) {
      obs = std::min(100.0f, std::max(0.0f, obs));
      const float dev = obs - profile_.holdPowerPct;
      const float profileHoldBefore = profile_.holdPowerPct;
#ifdef MAYAP_THERMAL_DEBUG_HOLD
      std::fprintf(stderr, "HOLDWIN t=%u obs=%.1f flat=%d profile=%.1f win=%us pv=%.2f\n", now / 1000U, obs, flat, profile_.holdPowerPct, windowMs / 1000U, in.pv);
#endif
      if (holdWindows_ == 0) profile_.holdPowerPct = obs;
      else {
        const bool forming = holdWindows_ < Policy::HoldFormingWindows || mismatch_;
        const float up = forming ? Policy::HoldMaxStepPct : Policy::HoldMaxRisePct;
        const float down = forming ? Policy::HoldMaxStepPct : Policy::HoldMaxFallPct;
        float step = std::min(up, std::max(-down, 0.3f * dev));
        holdConverging_ = false;
        if (smartHold_ && !forming && dev > 0.0f) {
          // The equilibrium duty of a flat window is a measurement, not an estimate: two of them that agree prove the
          // hold is higher than the profile. Move toward 92 % of the LOWER one (never past a measurement), faster than
          // the 1 pp/window noise guard that is right for a single window.
          if (flat && holdPrevFlat_ && std::isfinite(holdPrevObs_)) {
            const float lo = std::min(obs, holdPrevObs_);
            const float target = Policy::HoldFastFraction * lo;
            if (std::fabs(obs - holdPrevObs_) <= std::max(Policy::HoldFastAgreePct, 0.10f * lo) && target > profile_.holdPowerPct)
              step = std::max(step, std::min(Policy::HoldFastRisePct, 0.5f * (target - profile_.holdPowerPct)));
          }
          // Still catching up with the measurement after this move. Only before the profile has ever agreed with two flat
          // windows: afterwards a higher equilibrium duty IS evidence of a plant change (weaker heater, colder room).
          holdConverging_ = !holdSettled_ && step < dev * 0.9f;
        }
        profile_.holdPowerPct += step;
      }
      if (smartHold_) {
        if (flat && std::fabs(obs - profile_.holdPowerPct) <= std::max(Policy::HoldFastAgreePct, 0.10f * obs)) {
          if (holdAgreeStreak_ < 255) ++holdAgreeStreak_;
          if (holdAgreeStreak_ >= 2) holdSettled_ = true;
        } else if (flat) holdAgreeStreak_ = 0;
      }
      holdPrevObs_ = obs; holdPrevFlat_ = flat;
      // Plant-change evidence: FLAT windows only (equilibrium duty is a direct measurement), compared
      // with a FIXED reference taken when a deviating run starts.
      if (flat && holdWindows_ >= Policy::HoldFormingWindows && !(smartHold_ && holdConverging_)) evaluateHoldWindow(obs, profileHoldBefore, 100.0f * Policy::SensorLsbC / (std::max(profile_.heaterGain, Limits::GainMin) * spanS));
      ++holdWindows_;
    }
    hold_.active = false;  // next window starts on the next sample
  }

  // ---- derived estimates -------------------------------------------------------------------
  void updateDerived() {
    if (sxxS_[best_] >= Policy::SxxMin) {
      const float kh = sxyS_[best_] / (sxxS_[best_] + 0.05f);
      if (std::isfinite(kh) && kh > 0) {
        // bounded slow move (EMA) into the profile: no single window can jerk it
        const float target = std::min(Limits::GainMax, std::max(Limits::GainMin, kh));
        const float a = profileFresh_ ? 0.08f : 0.35f;
        profile_.heaterGain += a * (target - profile_.heaterGain);
        profileFresh_ = true;
      }
      // parabolic refinement of the delay around the best candidate
      float d = DelayGrid[best_];
      if (best_ > 0 && best_ + 1U < Cands && sxxS_[best_ - 1U] >= Policy::SxxMin &&
          sxxS_[best_ + 1U] >= Policy::SxxMin) {
        const float y0 = sxyS_[best_ - 1U] * sxyS_[best_ - 1U] / (sxxS_[best_ - 1U] + 1e-6f);
        const float y1 = sxyS_[best_] * sxyS_[best_] / (sxxS_[best_] + 1e-6f);
        const float y2 = sxyS_[best_ + 1U] * sxyS_[best_ + 1U] / (sxxS_[best_ + 1U] + 1e-6f);
        const float den = y0 - 2.0f * y1 + y2;
        if (den < -1e-9f) {
          const float shift = 0.5f * (y0 - y2) / den;  // in grid cells, within (-1,1)
          const float x = std::min(1.0f, std::max(-1.0f, shift));
          const float lo = DelayGrid[best_ - 1U], hi = DelayGrid[best_ + 1U];
          d = x < 0 ? d + x * (d - lo) : d + x * (hi - d);
        }
      }
      // The delay is published only once the fit is decisive; until then the prior stands.
      if (delayScore_ >= Policy::MinDelayScore)
        profile_.heaterDelaySec += 0.15f * (std::min(Limits::DelayMax, std::max(0.0f, d)) - profile_.heaterDelaySec);
    }
    // coast: the existing observer's measured value refines the model estimate (reuse)
    const float modelCoast = std::min(Limits::CoastRiseMax,
                                      profile_.heaterGain * (profile_.heaterDelaySec + 8.0f) * 0.9f);
    float coast = modelCoast;
    float coastSec = std::min(Limits::CoastTimeMax, 1.15f * profile_.heaterDelaySec + 12.0f);
    if (hint_.valid && hint_.coastWindows >= 1 && hint_.coast >= 0.0f) {
      coast = std::min(Limits::CoastRiseMax, 0.5f * modelCoast + 0.5f * hint_.coast);
      if (hint_.coastSec > 0) coastSec = 0.5f * coastSec + 0.5f * std::min(Limits::CoastTimeMax, hint_.coastSec);
    }
    coastScale_ = 1.0f;
    if (hint_.valid && hint_.coastWindows >= 1 && modelCoast > 0.05f && hint_.coast > modelCoast)
      coastScale_ = std::min(3.0f, hint_.coast / modelCoast);
    profile_.coastRiseC += 0.2f * (coast - profile_.coastRiseC);
    profile_.coastTimeSec += 0.2f * (coastSec - profile_.coastTimeSec);
  }

  float fitScore() const {  // prediction quality 0..1
    return std::min(1.0f, std::max(0.0f, 1.0f - pe_ / 1.6f));
  }
  void updateConfidence() {
    const float info = std::min(1.0f, sxxS_[best_] / Policy::SxxTarget);
    gainScore_ = info;
    holdScore_ = holdWindows_ >= 3 ? 1.0f : holdWindows_ == 2 ? 0.8f : holdWindows_ == 1 ? 0.5f : 0.0f;
    const float aging = std::min(1.0f, profile_.validLearningSec / 1800.0f);
    // Gain and delay are the backbone: both must be established. Hold and aging refine.
    float c = 100.0f * (0.40f * gainScore_ * fitScore() + 0.22f * delayScore_ + 0.20f * holdScore_ + 0.18f * aging);
    if (mismatch_) c *= 0.5f;
    if (profile_.validLearningSec < Policy::MinValidLearningSec) c = std::min(c, 25.0f);
    const uint8_t target = static_cast<uint8_t>(std::min(100.0f, std::max(0.0f, c)));
    // rise slowly, fall quickly (conservative)
    if (target > profile_.confidence) {
      const int step = std::max(1, (target - profile_.confidence) / 20);
      profile_.confidence = static_cast<uint8_t>(std::min<int>(target, profile_.confidence + step));
    } else if (target < profile_.confidence) {
      const int step = std::max(1, (profile_.confidence - target) / 6);
      profile_.confidence = static_cast<uint8_t>(std::max<int>(target, profile_.confidence - step));
    }
    updateState();
  }
  void updateState() {
    LearnState s;
    const uint8_t c = profile_.confidence;
    if (mismatch_) s = LearnState::Degraded;
    else if (wasDegraded_ && c < 60) s = LearnState::Adapting;
    else if (c >= 60) s = LearnState::Qualified;
    else if (profile_.validLearningSec == 0 && c == 0 && !seeded_) s = LearnState::Unlearned;
    else s = LearnState::Learning;
    if (s == LearnState::Degraded) wasDegraded_ = true;
    else if (s == LearnState::Qualified) wasDegraded_ = false;
    profile_.state = static_cast<uint8_t>(s);
  }

  // ---- ventilation event estimator ----------------------------------------------------------
  void ventCancel() { vent_.phase = 0; }
  void trackVent(uint32_t now, const LearnInput &in, GateReason g, uint32_t dt) {
    // The vent experiment tolerates the vent/recovery gates themselves but not real faults.
    const bool faultGate = g == GateReason::Sensor || g == GateReason::Safety || g == GateReason::HeaterBlocked ||
                           g == GateReason::Tune || g == GateReason::Test || g == GateReason::Maintenance ||
                           g == GateReason::Recovery || g == GateReason::Gap || g == GateReason::Jump ||
                           g == GateReason::Door || g == GateReason::Fan;
    if (faultGate && g != GateReason::Fan) { ventCancel(); }
    if (!in.fanStable) { ventCancel(); return; }
    if (in.ventActive && !ventOn_) {  // rising edge
      vent_ = VentEvent{};
      // pre-windows must have been vent-free and clean: last 2W
      const uint32_t quietFor = ventOffAt_ ? now - ventOffAt_ : 0xffffffffU;
      if (!faultGate && cleanSince_ != 0U && now - cleanSince_ >= 2U * Policy::WindowMs &&
          quietFor >= Policy::VentRecoveryMs) {
        vent_.phase = 1; vent_.t0 = now;
        slopeAt(now, vent_.sPre);
        vent_.dPre = meanDutyDelayed(now);
        vent_.preOk = std::isfinite(vent_.dPre);
      }
      return;
    }
    if (!in.ventActive) { vent_.phase = 0; return; }
    if (vent_.phase == 1 && !faultGate) {
      const uint32_t te = vent_.t0 + Policy::VentSettleMs + Policy::WindowMs;
      if (static_cast<int32_t>(now - te) >= 0) {
        float sDur;
        if (slopeAt(te, sDur) && vent_.preOk) {
          const float dDur = meanDutyDelayed(te);
          const float dd = dDur - vent_.dPre;
          const bool heaterMoved = std::fabs(dd) > 0.10f;
          if (!heaterMoved || gainTrust() >= 0.4f) {
            float obs = -((sDur - vent_.sPre) - profile_.heaterGain * dd);
            applyVent(obs);
          }
        }
        vent_.phase = 2;  // one measurement per event
      }
    } else if (vent_.phase == 1 && faultGate) {
      vent_.phase = 0;
    }
    (void)dt;
  }
  float meanDutyDelayed(uint32_t te) const {
    const uint32_t d = static_cast<uint32_t>(profile_.heaterDelaySec * 1000.0f);
    float u;
    if (!meanDuty(te - d - Policy::WindowMs, te - d, u)) return NAN;
    return u;
  }
  void applyVent(float observed) {
    if (!std::isfinite(observed)) return;
    // only cooling is a valid vent effect; a "heating" vent event is noise/disturbance
    const float obs = std::min(Limits::VentGainMax, std::max(0.0f, observed));
    if (ventEvents_ == 0) {
      profile_.ventCoolingGain = obs;
      ventStreak_ = 0;
    } else {
      const float cur = profile_.ventCoolingGain;
      const float ratio = (obs + 1e-5f) / (cur + 1e-5f);
      if (ratio > 2.0f || ratio < 0.5f) {
        ++ventStreak_;
        if (ventStreak_ >= 2) {  // consistent change in fan effectiveness: follow it
          profile_.ventCoolingGain += 0.6f * (obs - cur);
          ventStreak_ = 0;
          profile_.ventConfidence = static_cast<uint8_t>(profile_.ventConfidence * 0.6f);
        } else {
          profile_.ventConfidence = static_cast<uint8_t>(profile_.ventConfidence * 0.8f);
        }
      } else {
        ventStreak_ = 0;
        profile_.ventCoolingGain += 0.25f * (obs - cur);
        profile_.ventConfidence = static_cast<uint8_t>(std::min<int>(100, profile_.ventConfidence + 12));
      }
    }
    ++ventEvents_;
    if (ventEvents_ == 1) profile_.ventConfidence = 20;
    if (ventEvents_ >= 2 && ventStreak_ == 0 && profile_.ventConfidence < 30) profile_.ventConfidence = 30;
    profile_.ventCoolingGain = std::min(Limits::VentGainMax, std::max(0.0f, profile_.ventCoolingGain));
  }

  struct HoldWindow {
    bool active = false;
    uint32_t t0 = 0, delayMs = 0;
    Cum cumDelayed0 = 0;
    float pv0 = 0, lo = 0, hi = 0;
  };
  struct VentEvent {
    uint8_t phase = 0;  // 0 none, 1 waiting for the during-window, 2 measured
    uint32_t t0 = 0;
    float sPre = 0;
    float dPre = NAN;
    bool preOk = false;
  };

  ThermalProfile profile_{};
  bool enabled_ = false, seeded_ = false, profileFresh_ = false, holdSeen_ = false;
  bool tickSeen_ = false, sampleSeen_ = false, gapSeen_ = false, ventOn_ = false;
  bool mismatch_ = false, wasDegraded_ = false, slopeValid_ = false;
  uint8_t mismatchCause_ = 0;  // 1 gain ratio, 2 hold power
  float slope90_ = 0, coastScale_ = 1.0f;
  uint32_t tickAt_ = 0, sampleAt_ = 0, cumOnMs_ = 0, cleanSince_ = 0, settleUntil_ = 0, validMs_ = 0, ventOffAt_ = 0, lastMismatchAt_ = 0;
  float lastPv_ = NAN, lastRaw_ = NAN, lastSp_ = NAN;
  GateReason gate_ = GateReason::Disabled;
  Entry ring_[RingSize]{};
  uint8_t head_ = 0, count_ = 0;
  uint32_t newestT_ = 0, newestCum_ = 0;
  float sxxS_[Cands]{}, sxyS_[Cands]{}, sxxF_[Cands]{}, sxyF_[Cands]{};
  float syyS_ = 0, gainFast_ = 0, rVar_ = 0, pe_ = 2.0f, gainScore_ = 0, delayScore_ = 0, holdScore_ = 0;
  uint8_t best_ = 4;
  uint16_t mismatchRun_ = 0, recoverRun_ = 0, outlierRun_ = 0, holdMismatchRun_ = 0, holdAgreeRun_ = 0;
  int holdDevSign_ = 0;
  uint32_t mismatchByReason_[4]{};
  float holdRef_ = 0, holdDevVar_ = 0, lastHoldObs_ = 0;
  bool holdDevSeen_ = false, holdObsSeen_ = false;
  uint32_t fastUpdateAt_ = 0;   // sampleAt_ of the last full-weight (open-loop-like) update of the fast accumulators
  bool smartHold_ = (MAYAP_SMART_THERMAL != 0), holdPrevFlat_ = false, holdConverging_ = false, holdSettled_ = false;
  uint8_t holdAgreeStreak_ = 0;
  float holdPrevObs_ = NAN;
  uint32_t updates_ = 0, outliers_ = 0, mismatchEvents_ = 0, ventEvents_ = 0;
  uint8_t ventStreak_ = 0;
  Hints hint_{};
  VentEvent vent_{};
  HoldWindow hold_{};
  uint32_t holdWindows_ = 0;
};

}  // namespace MayapThermal
