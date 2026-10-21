#pragma once
#include "thermal_assist.h"
#include "thermal_profile.h"

// Smart Thermal startup (self-calibrated heat-in-flight brake, see ThermalStartupController::setSmart).
// Follows MAYAP_SMART_THERMAL (default 0 = legacy behaviour; see thermal_assist.h); can be forced alone with
// -DMAYAP_SMART_STARTUP=1. Rollback is the same switch. EXCEPTION (2026-10-15, doc/SLOW_OVEN_STUDY.md): the slow-oven hold learner in
// ThermalStartupController::decide() (delay > 80 s, PV flat just under SP) is NOT behind the flag: the default build needs it, the field
// oven sat under SP without it. A plant with a short delay (<= 80 s) takes the unchanged legacy branch.
#ifndef MAYAP_SMART_STARTUP
#define MAYAP_SMART_STARTUP MAYAP_SMART_THERMAL
#endif

// Pure thermal algorithms. Including code provides MachineConfig, timing,
// constants and sanitizeMachineConfig; the host test runs these SAME classes.
//
// targetTemp is deliberately NOT part of this predicate. A setpoint edit is a
// control command and must reach the P term on the next sensor sample; treating
// it as a "bumpless config" change would back-calculate I to cancel that P step.
inline bool thermalPidRuntimeConfigChanged(const MachineConfig &before,
                                           const MachineConfig &after) {
  return before.controlMode != after.controlMode ||
         before.kp != after.kp ||
         before.ki != after.ki ||
         before.kd != after.kd ||
         before.maxHeaterPower != after.maxHeaterPower;
}

class ThermalController {
 public:
  explicit ThermalController(float beta = THERMAL_PID_BETA)
      : beta_(clampFloat(beta, 0.0f, 1.0f)) {}
  void reset() {
    initialized_ = false;
    adoptFf_ = false;
    integral_ = 0.0f;
    lastInput_ = 0.0f;
    lastComputeAt_ = 0;
    output_ = 0.0f;
    filteredDerivative_ = 0.0f;
    ffApplied_ = 0.0f;
    addApplied_ = 0.0f;
    lastCorrection_ = 0.0f; lastLo_ = 0.0f; lastHi_ = 100.0f; lastHoldFf_ = 0.0f; lastVentFf_ = 0.0f;   // telemetry follows the reset
  }

  // Ap dung cau hinh moi ma giu nguyen cong suat hien tai. Cach nay tranh
  // nha contactor tong chi vi nguoi dung sua/lưu mot thong so tren HMI.
  void applyConfigBumpless(uint32_t now, float setpoint, float input,
                           const MachineConfig &cfg) {
    if (!initialized_ || !isfinite(input)) return;
    const float maxOut = static_cast<float>(cfg.maxHeaterPower);
    output_ = clampFloat(output_, 0.0f, maxOut);
    lastInput_ = input;
    lastComputeAt_ = now;
    filteredDerivative_ = 0.0f;
    if (cfg.controlMode == ControlMode::Pid) {
      const float integralLimit = maxOut + fabsf(cfg.kp * (1.0f - beta_) * setpoint);
      integral_ = clampFloat(output_ - ffApplied_ - addApplied_ - cfg.kp * (beta_ * setpoint - input),
                            -integralLimit, integralLimit);
    } else {
      integral_ = 0.0f;
    }
  }

  // Take over from a source that has been delivering `outputPct` on average (relay hand-over): start the
  // controller already holding that output, integral projected from it, so the first cycle is no step.
  void seedBumpless(uint32_t now, float setpoint, float input, const MachineConfig &cfg, float outputPct) {
    reset();
    if (!isfinite(input) || !isfinite(setpoint) || cfg.controlMode != ControlMode::Pid) return;
    const float maxOut = static_cast<float>(cfg.maxHeaterPower);
    output_ = clampFloat(isfinite(outputPct) ? outputPct : 0.0f, 0.0f, maxOut);
    initialized_ = true;
    lastInput_ = input;
    lastComputeAt_ = now;
    const float integralLimit = maxOut + fabsf(cfg.kp * (1.0f - beta_) * setpoint);
    integral_ = clampFloat(output_ - cfg.kp * (beta_ * setpoint - input), -integralLimit, integralLimit);
  }

  // The launch drives the heater itself (boost): the integral starts over at the feed-forward, so the hold the launch measured is the
  // operating point of the hand-back and the boost is not carried into it as windup.
  void restartIntegral() { integral_ = 0.0f; adoptFf_ = true; }

  // The startup controller may REQUEST a power (boost): the PID then follows it bumplessly, so the hand-back is no step.
  void forceOutputBumpless(float setpoint, float input, const MachineConfig &cfg, float outputPct) {
    if (!initialized_ || !isfinite(input) || !isfinite(setpoint) || cfg.controlMode != ControlMode::Pid) return;
    const float maxOut = static_cast<float>(cfg.maxHeaterPower);
    output_ = clampFloat(isfinite(outputPct) ? outputPct : 0.0f, 0.0f, maxOut);
    const float integralLimit = maxOut + fabsf(cfg.kp * (1.0f - beta_) * setpoint);
    integral_ = clampFloat(output_ - ffApplied_ - addApplied_ - cfg.kp * (beta_ * setpoint - input), -integralLimit, integralLimit);
  }

  float updateOnNewSample(uint32_t now, float setpoint, float input,
                          const MachineConfig &cfg, bool enabled,
                          float actuatorCeiling = INFINITY, bool freezePositiveIntegral = false,
                          const MayapThermal::Assist *assist = nullptr) {
    if (!enabled || !isfinite(input) || !isfinite(setpoint)) { reset(); return 0.0f; }
    const float maxOut = isfinite(actuatorCeiling)
        ? clampFloat(actuatorCeiling, 0.0f, static_cast<float>(cfg.maxHeaterPower))
        : static_cast<float>(cfg.maxHeaterPower);
    if (cfg.controlMode == ControlMode::OnOff) {
      const float half = cfg.tempHysteresis * 0.5f;
      if (!initialized_) { output_ = input < setpoint ? maxOut : 0.0f; initialized_ = true; }
      else if (input <= setpoint - half) output_ = maxOut;
      else if (input >= setpoint + half) output_ = 0.0f;
      lastInput_ = input; lastComputeAt_ = now;
      return output_;
    }

    // Adaptive Thermal V1 feed-forward (all-default assist == 0 == legacy).
    // The feed-forward state follows the REQUESTED value (bounded to 0..100), not the clamped
    // one: a transient ceiling (startup brake) must not make the FF look "removed", or the
    // bumpless transfer would later subtract it from the integral and cancel its purpose.
    const float ff = assist ? clampFloat(assist->feedForward, 0.0f, 100.0f) : 0.0f;
    // Known-disturbance feed-forward (vent): new load, so it is added on top and never traded
    // with the integral (which would cancel it).
    const float addFf = assist ? clampFloat(assist->addForward, 0.0f, 100.0f) : 0.0f;
    if (!initialized_) {
      initialized_ = true;
      lastInput_ = input;
      lastComputeAt_ = now;
      integral_ = 0.0f;
      // A (re)started controller begins from "heater off": the feed-forward applies at once
      // instead of being subtracted from the integral, which would cancel its purpose.
      ffApplied_ = ff;
      addApplied_ = addFf;
      output_ = clampFloat(ff + addFf + cfg.kp * (beta_ * setpoint - input), 0.0f, maxOut);
      return output_;
    }

    float dt = static_cast<float>(elapsedMs(now, lastComputeAt_)) * 0.001f;
    if (dt <= 0.0f) return output_;
    dt = clampFloat(dt, 0.25f, 10.0f);
    lastComputeAt_ = now;
    const float error = setpoint - input;
    const float dInput = (input - lastInput_) / dt;
    lastInput_ = input;

    // (the proportional ceiling narrows the error part only: the set point weighting's constant stays, so a change of the ceiling is no step)
    const float kpEff = (assist && isfinite(assist->kpMax)) ? fminf(cfg.kp, fmaxf(0.0f, assist->kpMax)) : cfg.kp;
    const float p = cfg.kp * (beta_ * setpoint - input) - (cfg.kp - kpEff) * (setpoint - input);
    // First-order derivative filter: sensor noise must not command full SSR
    // swings. Derivative stays on PV, avoiding setpoint derivative kick.
    filteredDerivative_ += (dt / (PID_D_FILTER_TAU_SEC + dt)) *
                           (dInput - filteredDerivative_);
    const float d = -(cfg.kp > 0.0f ? cfg.kd * kpEff / cfg.kp : cfg.kd) * filteredDerivative_;
    // Weighted absolute Celsius P has a DC offset. Permit I to cancel it:
    // the old +/-maxOut bound alone can prevent beta<1 reaching the setpoint.
    // Anti-windup uses the ACTUAL 0..maxOut actuator limits.
    const float integralLimit = maxOut + fabsf(cfg.kp * (1.0f - beta_) * setpoint);
    // The integral carries the RESIDUAL: when the feed-forward moves, the integral moves
    // the opposite way so the TOTAL is continuous (bumpless, no double counting of hold).
    if (adoptFf_) { ffApplied_ = ff; adoptFf_ = false; }
    if (ff != ffApplied_) {
      integral_ = clampFloat(integral_ - (ff - ffApplied_), -integralLimit, integralLimit);
      ffApplied_ = ff;
    }
    const bool freeze = assist && assist->freezeIntegral;
    const float integralCeil = assist ? assist->integralCeiling : INFINITY;
    // Learned-model ceiling on Ki (V1): lowers an over-aggressive integral for a slow/delayed
    // plant; it never raises the configured gain.
    const float kiEff = (assist && isfinite(assist->kiMax)) ? fminf(cfg.ki, fmaxf(0.0f, assist->kiMax)) : cfg.ki;
    float integralDelta = kiEff * error * dt;
    // Vent window: the known disturbance belongs to the feed-forward; I may only unwind.
    if (freeze && integralDelta > 0.0f) integralDelta = 0.0f;
    if (freezePositiveIntegral && integralDelta > 0.0f) integralDelta = 0.0f;
    float candidateIntegral = clampFloat(integral_ + integralDelta, -integralLimit, integralLimit);
    // The ceiling blocks growth only: an integral already above it may still unwind.
    if (isfinite(integralCeil))
      candidateIntegral = fminf(candidateIntegral, fmaxf(integralCeil, integral_));
    const float pp = p + ff + addFf;  // feed-forward shifts the proportional operating point
    // Output bounds: legacy [0, maxOut], optionally narrowed around the feed-forward by the
    // V1 correction-authority limiter (never widened).
    float lo = 0.0f, hi = maxOut;
    if (assist && isfinite(assist->corrBase)) {
      const float authority = fmaxf(0.0f, assist->corrBase) + fmaxf(0.0f, assist->corrPerC) * fabsf(error);
      lo = fmaxf(0.0f, ff + addFf - authority);
      hi = fminf(maxOut, ff + addFf + authority);
      if (lo > hi) lo = hi;
    }
    const float unsaturated = pp + candidateIntegral + d;
    const float previousUnsaturated = pp + integral_ + d;
    const float integralStep = candidateIntegral - integral_;
    // A step crossing a limit must reach that limit. Discarding the whole
    // step can leave positive heat indefinitely while PV is above SP.
    if (unsaturated < lo && integralStep < 0.0f && previousUnsaturated > lo) {
      integral_ = clampFloat(lo - pp - d, -integralLimit, integralLimit);
    } else if (unsaturated > hi && integralStep > 0.0f &&
               previousUnsaturated < hi) {
      integral_ = clampFloat(hi - pp - d, -integralLimit, integralLimit);
    } else if ((unsaturated >= lo && unsaturated <= hi) ||
               (unsaturated > hi && integralStep < 0.0f) ||
               (unsaturated < lo && integralStep > 0.0f)) {
      integral_ = candidateIntegral;
    }
    addApplied_ = addFf;
    output_ = clampFloat(pp + integral_ + d, lo, hi);
    // Read-only telemetry (PID monitor): the correction is P + I + D with the feed-forwards REMOVED, so they are never counted twice.
    lastCorrection_ = p + integral_ + d;
    lastLo_ = lo; lastHi_ = hi;
    lastHoldFf_ = ff; lastVentFf_ = addFf;
    return output_;
  }

  float output() const { return output_; }
  float lastCorrection() const { return lastCorrection_; }
  float lastLimitLo() const { return lastLo_; }
  float lastLimitHi() const { return lastHi_; }
  float lastHoldFf() const { return lastHoldFf_; }
  float lastVentFf() const { return lastVentFf_; }
  float integral() const { return integral_; }
  float feedForwardApplied() const { return ffApplied_; }

 private:
  const float beta_;
  bool initialized_ = false;
  bool adoptFf_ = false;   // the next feed-forward value is adopted without shifting the integral
  float integral_ = 0.0f;
  float filteredDerivative_ = 0.0f;
  float lastInput_ = 0.0f;
  uint32_t lastComputeAt_ = 0;
  float output_ = 0.0f;
  float ffApplied_ = 0.0f;
  float addApplied_ = 0.0f;  // additive (vent) feed-forward contained in output_
  float lastCorrection_ = 0.0f, lastLo_ = 0.0f, lastHi_ = 100.0f, lastHoldFf_ = 0.0f, lastVentFf_ = 0.0f;   // telemetry only
};

// Physical heat is metered from the arbiter's actual SSR state, not PID demand.
// The bounded 2 s buckets retain 240 s of energy and no credit across a cut.
class ThermalStartupController {
 public:
  enum class Phase : uint8_t { FullHeat, Approach, SoftLanding, Hold };
  void reset() {
    // The observation window is lost (a gap, a trip, a mode change), the plant is not: what this launch measured becomes the prior of the next
    // one, as the stored plan would after a restart (the plan of the last good warm-up is kept if this one measured nothing).
    if (gainInfo_ && onset_ && gainEst_ > 0.0f) {
      retained_.valid = true; retained_.gain = clampFloat(gainEst_, 0.0005f, 0.5f);
      retained_.delaySec = clampFloat(lastDhS_, 0.0f, DelayUnknownS);
      retained_.coastCal = calKnown_ ? coastCal_ : DefaultPlanCal; retained_.calMeasured = calKnown_;
      retained_.holdPct = (holdValid_ && holdLive_ > 0.0f) ? clampFloat(holdLive_, 0.0f, 100.0f) : retained_.holdPct;
    }
    for (uint8_t i = 0; i < Buckets; ++i) onMs_[i] = 0;
    phase_ = Phase::FullHeat; initialized_ = false; observed_ = false; historyGap_ = false;
    bucket_ = 0; firstHeatAt_ = 0; firstRiseAt_ = 0;
    lastHeatOffAt_ = 0; energyWindowStartedAt_ = 0; coastCleared_ = false;
    stableAt_ = 0; lastRequested_ = 0; holdPower_ = 0;
    slope_ = 0; lastSampleAt_ = 0; lastPeak_ = 0; khObs_ = 0; pvSlow_ = 0; flatMs_ = 0; flatMarkAt_ = 0U; flatMarkPv_ = 0.0f; holdMeasured_ = -1.0f; onTotalMs_ = 0U; pvStart_ = 0.0f; pvStartSet_ = false; warm_ = false; gainHistBid_ = 0U; gainHistInit_ = false;
    for (uint8_t i = 0; i < GainHistLen; ++i) { gainHist_[i] = 0U; pvHist_[i] = 0.0f; }
    pvFast_ = 0.0f; gainLive_ = 0.0f; gainEst_ = 0.0f; gainInfo_ = false; holdLive_ = -1.0f; holdIdle_ = -1.0f; holdFlat_ = -1.0f; holdValid_ = false; coastCal_ = 1.0f; calKnown_ = false; planBad_ = false;
    onset_ = false; lastDhS_ = DelayUnknownS; lastGUseS_ = ProbeGain; onsetElapsedS_ = 0.0f; pvMin_ = 0.0f; pvMinSet_ = false; pvcMin_ = 0.0f; pvcMinSet_ = false; pvMinAt_ = 0U; lambda0_ = 0.0f; lambdaT_ = 0.0f; onsetSeen_ = 0U;
    cutActive_ = false; cutDone_ = false; cutAt_ = 0U; cutPv_ = 0.0f; cutPred1_ = 0.0f; cutPeak_ = 0.0f; cutFlatAt_ = 0U; cutIfS_ = cutWinS_ = cutG_ = cutDrift_ = 0.0f; cutX_ = 1.0f; calHoldDone_ = false;
    planOffered_ = false; planReady_ = false; holdSince_ = 0U; lastBoostAt_ = 0U; overshootSeen_ = false; pvMaxAfterBoost_ = 0.0f;
    boostNow_ = false; trustedNow_ = false; holdFf_ = 0.0f; tiMin_ = 0.0f; kpMax_ = 0.0f; stallRelax_ = 0.0f; stallSince_ = 0U;
    hint_ = MayapThermal::StartupHint{};
  }
  void observe(uint32_t now, bool heaterOn) {
    if (!observed_) { observed_ = true; observedAt_ = bucketAt_ = now; lastOn_ = heaterOn; return; }
    uint32_t dt = static_cast<uint32_t>(now - observedAt_);
    observedAt_ = now;
    if (dt > 2000U) {
      reset(); historyGap_ = true;
      observed_ = true; observedAt_ = bucketAt_ = now; return;
    }
    while (dt) {
      const uint32_t left = 2000U - static_cast<uint32_t>(observedAt_ - dt - bucketAt_);
      const uint32_t part = dt < left ? dt : left;
      if (lastOn_) { onMs_[bucket_] += part; onTotalMs_ += part; }
      dt -= part;
      if (part == left) {
        bucketAt_ += 2000U;
        bucket_ = (bucket_ + 1U) % Buckets;
        onMs_[bucket_] = 0;
      }
    }
    if (lastOn_ && !heaterOn) { lastHeatOffAt_ = now; coastCleared_ = false; }
    lastOn_ = heaterOn;
    if (heaterOn && firstHeatAt_ == 0U) firstHeatAt_ = now;
  }
  // What the launch model says about the heat on its way (all zero for a learned-hint plant).
  struct Prior { bool valid = false; float gain = 0.0f, delaySec = 0.0f, holdPct = 0.0f; };
  struct LaunchView {
    float gUse = 0.0f;        // degC/s at 100 %: a ceiling of the heater gain until the launch has measured it
    float gEst = 0.0f;        // degC/s at 100 %: the measured gain (0 = not measured yet)
    float kappa = 1.0f;       // reserve on the modelled coast
    float inFlightS = 0.0f;   // ON-seconds weighted by the part of their heat that has not reached PV yet
    float windowS = 0.0f;     // the same window as seconds (the loss integrates over it)
    float holdCred = 0.0f;    // % of the bank credited against the in-flight heat (measured hold, else the plan's)
    float holdFull = 0.0f;    // % of the bank that holds PV at SP (measured on this launch, else the prior's), 0 = unknown
    float dhS = 0.0f;         // delay after which the heat of an ON-second starts to reach PV
    bool informative = false; // the gain is measured (response well above the noise), not just bounded
  };
  // floorPct > 0: the controller REQUESTS this power (accelerate-then-brake); 0: the PID owns the request below `ceiling`.
  struct Decision { float ceiling; bool freezePositiveIntegral; Phase phase; float peak; float floorPct; };
  // Launch plan of the last good warm-up (a prior; survives reset(), it is a configuration like the Smart flag).
  void setPlan(const MayapThermal::LaunchPlan &p) { plan_ = p; }
  // A vent or another disturbance is running: no gain / drift measurement is taken from this time.
  void setDisturbed(bool d) { disturbed_ = d; }
  // A plan worth saving (gain / delay / drift calibration measured on this warm-up), once per batch.
  bool takePlan(MayapThermal::LaunchPlan &out) { if (!planReady_) return false; out = planOut_; planReady_ = false; return true; }
  bool trusted() const { return trustedNow_; }
  bool boosting() const { return boostNow_; }
  bool cutting() const { return cutActive_; }
  // Feed-forward for the PID (% duty): the hold measured on this launch, else 80 % of the plan's. 0 until one of them exists.
  float holdFeedForward() const { return holdFf_; }
  bool holdMeasured() const { return holdValid_; }   // the feed-forward above comes from this launch's own energy balance (it outranks a learned profile)
  // Shortest integral time (s) the measured delay allows: an integral faster than this winds a delayed plant into a limit cycle. 0 = unknown.
  float minIntegralTimeSec() const { return tiMin_; }
  // Largest proportional gain the measured (else the plan's) gain and delay allow: Kp x gain/100 x (delay + lag) <= KpLoopK/100. 0 = unknown.
  float maxProportionalGain() const { return kpMax_; }
  float liveGain() const { return gainLive_; }
  float coastCal() const { return coastCal_; }
  bool calKnown() const { return calKnown_; }
  // Learned plant knowledge from Adaptive Thermal V1. An invalid hint is exactly the
  // legacy controller: it replaces the lightest-plant coast assumption, never adds heat.
  void setHint(const MayapThermal::StartupHint &h) { hint_ = h; }
  // Smart Thermal startup (feature flag, default OFF == bit-identical legacy). It only ever ADDS braking: the expected
  // coast becomes max(legacy, self-calibrated heat in flight). It survives reset() (a configuration, not a state).
  void setSmart(bool on) { smart_ = on; }
  bool smart() const { return smart_; }
  float observedGain() const { return khObs_; }
  Decision decide(uint32_t now, float sp, float pv, float maxPower) {
    if (!isfinite(sp) || !isfinite(pv)) return {0, true, phase_, pv, 0.0f};
    historyGap_ = false; // only a fresh real sensor sample can resume heat
    // A set point raised by more than SetpointRelaunchC is a new warm-up on the same oven: what was measured (gain, delay, hold, drift) stays,
    // the bookkeeping of the previous warm-up (the deliberate cut, the plan offer, the overshoot watch) starts again.
    if (initialized_ && sp - lastSp_ > SetpointRelaunchC) {
      cutActive_ = false; cutDone_ = calKnown_; planOffered_ = false; planReady_ = false; holdSince_ = 0U; overshootSeen_ = false;
      pvMaxAfterBoost_ = pv; stallRelax_ = 0.0f; stallSince_ = 0U;
    }
    if (!initialized_) {
      initialized_ = true; lastPv_ = pv; lastSp_ = sp;
      lastSampleAt_ = now; pvSlow_ = pv; pvFast_ = pv;
      { const MayapThermal::LaunchPlan &p0 = (plan_.valid && !planBad_) ? plan_ : retained_; if (p0.valid) { coastCal_ = clampFloat(p0.coastCal, CalMin, CalMax); calKnown_ = p0.calMeasured; } }
    }
    const float dt = clampFloat(static_cast<float>(static_cast<uint32_t>(now-lastSampleAt_))*0.001f, 0.25f, 10.0f);
    const float measuredRate = (pv-lastPv_)/dt;
    slope_ += dt/(10.0f+dt)*(measuredRate-slope_);
    lastPv_ = pv; lastSampleAt_ = now;
    pvSlow_ += dt/(SlowPvTauSec+dt)*(pv-pvSlow_);   // slow average: the robust "is PV really flat" reference (immune to 0.1 C steps)
    pvFast_ += dt/(PvFastTauSec+dt)*(pv-pvFast_);   // light smoothing of the probe quantization for the windowed slope / gain
    if (smart_) observeGain(now);
    trackOnset(now);
    if (sp != lastSp_) { stableAt_ = 0U; phase_ = Phase::Approach; lastSp_ = sp; }
    if (!pvStartSet_ && firstHeatAt_ != 0U) { pvStartSet_ = true; pvStart_ = pv; warm_ = (sp - pv) < WarmStartErrC && ((plan_.valid && !planBad_) || retained_.valid); }
    // First rise = the heat has surfaced. A strong oven shows it as a slope above 0.003 C/s; a weak or heavy one never reaches that slope (its
    // whole climb is 0.0005 C/s), and with no "first rise" the hold learner, the delay estimate and the measured gain below stay switched off for
    // good: the oven then sits under the set point forever (grid test: 0.5 C under SP for 3 h). A rise of FirstRiseC since the first heat (two
    // probe steps) counts as well.
    if (firstRiseAt_ == 0U && firstHeatAt_ != 0U && (slope_ > 0.003f || (pvStartSet_ && pv - pvStart_ >= FirstRiseC)) &&
        static_cast<uint32_t>(now-firstHeatAt_) >= 15000U) firstRiseAt_ = now;
    const uint32_t riseDelayMs = firstRiseAt_ == 0U ? 0U :
        static_cast<uint32_t>(firstRiseAt_-firstHeatAt_);
    const bool longDelay = riseDelayMs > 80000U;
    float coastSeconds = firstRiseAt_ == 0U ? 135.0f :
        clampFloat(static_cast<float>(riseDelayMs)*0.001f+14.0f, 18.0f, 135.0f);
    // Learned delay replaces the single-event estimate in proportion to the hint strength.
    const float hs = hint_.valid ? clampFloat(hint_.strength, 0.0f, 1.0f) : 0.0f;
    if (hs > 0.0f)
      coastSeconds += hs * (clampFloat(hint_.delaySec + 14.0f, 18.0f, 240.0f) - coastSeconds);
    snapshotRing(now);
    if (!coastCleared_ && !lastOn_ && lastHeatOffAt_ != 0U &&
        static_cast<uint32_t>(now-lastHeatOffAt_) >= static_cast<uint32_t>(coastSeconds*1000.0f) &&
        slope_ <= 0.001f) {
      for (uint8_t i = 0; i < Buckets; ++i) onMs_[i] = 0;
      energyWindowStartedAt_ = now;
      coastCleared_ = true;
    }
    // ---- Measured plant (launch): the heat still on its way is modelled from the heater's actual ON-time, an arrival kernel (pure delay
    // then a first-order lag) and the heater gain MEASURED during this launch (or the plan of the last good warm-up). Until there is evidence
    // the gain is bounded from above by what has NOT been seen: ON-time whose heat should have surfaced by now and did not, is a ceiling on
    // the gain; that ceiling opens the budget for the next burst step by step. A learned hint of strength >= 0.5 keeps its own model.
    const bool launch = true;
    const bool trusted = launch;
    trustedNow_ = trusted;
    // Prior of the plant before this launch has measured it: the plan of the last good warm-up, else a learned profile of high strength.
    Prior prior;
    const MayapThermal::LaunchPlan &pp = (plan_.valid && !planBad_) ? plan_ : retained_;
    if (pp.valid) { prior.valid = true; prior.gain = pp.gain; prior.delaySec = pp.delaySec; prior.holdPct = pp.holdPct; }
    else if (hs >= 0.5f && hint_.gainPerSec > 0.0f) { prior.valid = true; prior.gain = hint_.gainPerSec; prior.delaySec = hint_.delaySec; prior.holdPct = 0.0f; }
    // The oven did not answer as promised: well under SP, a real amount of heat given, and no rise after twice the delay of the plan. (A restart
    // at the set point, with the hold duty only, shows no rise by design.)
    if (plan_.valid && !planBad_ && !onset_ && !warm_ && firstHeatAt_ != 0U && sp-pv > PlanBadMinErrC && onTotalMs_ >= PlanBadMinOnMs &&
        static_cast<uint32_t>(now-firstHeatAt_) > static_cast<uint32_t>(plan_.delaySec*2000.0f) + 90000U) planBad_ = true;
    const LaunchView lv = launchView(now, prior);
    lastDhS_ = lv.dhS; lastGUseS_ = lv.gUse;
    holdFf_ = lv.holdFull > 0.0f ? (holdValid_ ? HoldFfShare : HoldPlanCredit) * lv.holdFull : 0.0f;
    tiMin_ = (onset_ || prior.valid) ? TiPerDelay * (lv.dhS + LagS) : 0.0f;
    {
      const float gKp = gainInfo_ ? gainEst_ : (prior.valid ? prior.gain : 0.0f);
      kpMax_ = gKp > 0.0f && tiMin_ > 0.0f ? KpLoopK / (gKp * (lv.dhS + LagS)) : 0.0f;
    }
    // Quantized sensors (0.1 C steps) make the 10 s IIR slope spike on every step, which
    // fakes coast. With a learned hint the window-regression slope replaces it.
    float slopeEff = (hs > 0.0f && hint_.slopeValid) ? slope_ + hs*(hint_.slopePerSec - slope_) : slope_;
    // Measured plant: the 60 s window slope (a 0.1 C probe step makes the 10 s IIR slope jump to 0.005..0.01 C/s with PV not moving).
    if (trusted) slopeEff = windowSlope(now);
    // Measured plant: the energy still in flight (below) already carries the dead time, the slope only has to cover the lag tail of the
    // heat that is arriving now. (Slope x (delay + 14 s) over-predicts as soon as the heater is cut: the slope decays while the heat arrives.)
    const float slopeCoast = fmaxf(0.0f, slopeEff) * (trusted ? LagS + 10.0f : coastSeconds);
    // The long-term loss-compensation duty is deliberately slow and cannot
    // be learned until after the first delayed heat response. It removes the
    // permanent offset of proportional-only braking without banking pulse
    // energy for a later burst.
    if (firstRiseAt_ != 0U &&
        static_cast<uint32_t>(now-firstRiseAt_) >= (longDelay ? 240000U : 180000U)) {
      if (!longDelay || coastCleared_ || sp-pv > 1.0f) {
        if (sp-pv > 0.08f && slope_ <= 0.003f)
          holdPower_ += dt/60.0f;
        else if (sp-pv < -0.05f || slope_ > 0.006f)
          holdPower_ -= 3.0f*dt/60.0f;
        holdPower_ = clampFloat(holdPower_,0.0f,maxPower);
      } else {
        // Long-delay plant just under the setpoint, heater not "cleared". A trickle of pulses keeps the heater from ever being OFF for
        // the whole coast time, so the gate above never opens there: the energy brake (sized for a 16 kW bank on a light chamber) then
        // holds a permanent offset below SP of about  hold x horizon x 0.13 C/s  and the hold is never learned (PV sits just under SP).
        // The hold IS measurable there, without waiting for a cleared heater: when PV has been flat (not rising) on a slow average for
        // SlowHoldFlatMs, the heat in flight is stationary and the mean ACTUAL duty equals the duty needed to stand still. That value
        // is taken as the hold (95 %, up only). PV not rising means the duty cannot exceed the hold by more than the allowed drift, so
        // this cannot teach a hold above the truth. The brake then permits the surplus that closes the remaining error; the last
        // offset is closed by a slow integrator (a few tenths of a percent per minute: the heat of such a step is still in flight for a
        // whole delay, so a wrong step cannot overshoot) that runs only while PV is flat. It is bounded by the duty measured while flat
        // (x1.5 + 2 points: "flat" still hides about 1.5 points of duty on a weak heater), because without a bound the integrator keeps
        // winding up while the PID, not the brake, is what limits the duty (a 1.9 % oven reached a 40 % hold in a few hours, which
        // switches the brake off for every duty below it).
        // Lowering the hold stays fast (more braking is the safe direction).
        const float dev = pv-pvSlow_;   // > 0 rising, < 0 falling (slow average: immune to 0.1 C probe steps)
        const bool flat = sp-pv > 0.08f && dev <= SlowHoldRiseC && dev >= -SlowHoldFallC;
        // Flat is judged over a whole window, not sample by sample: with a 0.1 C probe every step breaks a per-sample test for minutes
        // (the slow average needs ~3 time constants to forget a step), so a weak oven with a few percent of hold never stayed "flat" for
        // six minutes in a row and its hold was never measured. The slow average must have moved less than SlowHoldDriftC over the window.
        if (sp-pv <= 0.08f || slope_ > 0.006f) {
          flatMarkAt_ = 0U;
        } else if (flatMarkAt_ == 0U) {
          flatMarkAt_ = now; flatMarkPv_ = pvSlow_;
        } else if (static_cast<uint32_t>(now-flatMarkAt_) >= SlowHoldFlatMs) {
          const float drift = pvSlow_ - flatMarkPv_;
          const uint32_t markAt = flatMarkAt_;
          flatMarkAt_ = now; flatMarkPv_ = pvSlow_;
          if (drift <= SlowHoldDriftC && drift >= -SlowHoldDriftC) {
            // The duty is read over the flat window shifted back by the delay: PV that stood still over [mark, now] was fed by the heater
            // over [mark-lag, now-lag]. The last minutes of duty are still in flight (a brake that kept the heater OFF for a dead time also
            // leaves PV flat, and the ON burst that follows it must not be mistaken for the hold: a 90 s dead-time oven learned 70 %
            // instead of 7 % and overshot by 0.9 C).
            const uint32_t lagMs = std::min<uint32_t>(riseDelayMs + 30000U, 300000U);
            const uint32_t spanMs = static_cast<uint32_t>(now-markAt);
            const uint32_t newBid = (now - lagMs) / GainHistStepMs, oldBid = (now - spanMs - lagMs) / GainHistStepMs;
            if (spanMs >= 240000U && gainHistBid_ - oldBid < GainHistLen && newBid > oldBid) {
              const uint32_t onMs = gainHist_[newBid % GainHistLen] - gainHist_[oldBid % GainHistLen];
              const float dutyPct = 100.0f*static_cast<float>(onMs)/static_cast<float>((newBid - oldBid)*GainHistStepMs);
              holdMeasured_ = fminf(maxPower, SlowHoldMeasuredFrac*dutyPct);
              holdPower_ = fmaxf(holdPower_, holdMeasured_);
            }
          }
        }
        if (flat) {
          if (holdMeasured_ >= 0.0f && holdPower_ < SlowHoldCeilFactor*holdMeasured_ + SlowHoldCeilPct)
            holdPower_ += SlowHoldPctPerMin*dt/60.0f;
        } else {
          // lowering follows the size of the overshoot: a probe step above SP (0.05..0.3 C) must not wipe a hold that took half an hour
          // to build, a real overshoot or a fast rise cuts at the full 3 %/min
          const float cut = slope_ > 0.006f ? 1.0f : clampFloat((pv-sp-0.05f)/SlowHoldCutSpanC, 0.0f, 1.0f);
          holdPower_ -= 3.0f*cut*dt/60.0f;
        }
        holdPower_ = clampFloat(holdPower_,0.0f,maxPower);
      }
    }
    // Learned hold power (average ACTUAL duty at the setpoint) replaces the slow legacy
    // integrator in proportion to the hint strength.
    const float holdBase = hs > 0.0f ? holdPower_ + hs*(clampFloat(hint_.holdPct,0.0f,maxPower)-holdPower_) : holdPower_;
    // A known vent removes heat: the duty needed just to stand still is higher while it runs.
    const float holdEff = clampFloat(holdBase + (hs > 0.0f ? hs*hint_.ventPct : 0.0f), 0.0f, maxPower);
    float excessOnMs, modelGain = 0.0f, holdModel = holdEff, excessNominalS = 0.0f;
    float modelEnergyCoast;
    if (launch) {
      // Heat in flight (ON-seconds weighted by the part of their heat that has not reached PV yet) less the loss over the same window, times
      // the heater gain (a ceiling while the launch has not measured it) and the model reserve. Far below SP the loss is a little smaller.
      const float x = clampFloat(1.0f - (sp-pv)/12.0f, 0.5f, 1.0f);
      const float credit = lv.holdCred * x;
      const float excessS = fmaxf(0.0f, lv.inFlightS - credit * 0.01f * lv.windowS);
      excessOnMs = excessS * 1000.0f;
      excessNominalS = excessS;
      modelGain = lv.gUse;
      modelEnergyCoast = excessS * lv.gUse * lv.kappa;
      holdModel = lv.holdCred;
    } else {
      uint32_t horizonMs = longDelay ? 240000U : 160000U;
      if (hs >= 0.5f)  // only energy still IN FLIGHT can coast: window = learned delay + margin
        horizonMs = static_cast<uint32_t>(clampFloat((hint_.delaySec+6.0f)*0.5f, 6.0f, 120.0f))*2000U;
      const uint8_t recentBuckets = static_cast<uint8_t>(horizonMs/2000U);
      uint32_t recentOnMs = 0;
      for (uint8_t i = 0; i < recentBuckets; ++i)
        recentOnMs += onMs_[(bucket_+Buckets-i)%Buckets];
      const float windowMs = static_cast<float>(std::min<uint32_t>(horizonMs,
          static_cast<uint32_t>(now-energyWindowStartedAt_)));
      excessOnMs = fmaxf(0.0f, static_cast<float>(recentOnMs) - holdEff * 0.01f * windowMs);
      // Rated watts are not a calibration of transfer to this one probe.
      // Keep a bounded 35% reserve for heater effectiveness, sensor filter lag
      // and delayed heat before trusting a first-rise estimate.
      // Do not subtract PV rise over this ring: with transport delay, that rise
      // can be caused by an older pulse which already aged out of the ring.
      modelEnergyCoast = excessOnMs*16.0f*1.35f/MinimumCapacity;
    }
    // A learned hint can only ADD braking to the legacy model. The measured model of the launch is not touched by it: the hint's coast figure is
    // per ON-ms of an average warm-up and double counts the heat the launch model already follows (limit cycle on a strong plant, 1 C overshoot).
    const float energyCoast = !launch && hs > 0.0f && hint_.coastPerOnMs > 0.0f
        ? fmaxf(modelEnergyCoast, modelEnergyCoast + hs*(excessOnMs*hint_.coastPerOnMs - modelEnergyCoast))
        : modelEnergyCoast;
    float expectedCoast = fmaxf(slopeCoast, energyCoast);
    // Backstop: a model that keeps predicting heat to arrive while PV has stood still for longer than any heat can be on its way is wrong
    // (too pessimistic a gain or a missing hold credit). The energy part of the prediction is relaxed, slowly, until PV moves again: a
    // stall costs minutes, it never lasts for hours.
    {
      const float dtS = dt;
      const bool stalled = launch && !cutActive_ && (sp-pv) > StallMinErrC && windowSlope(now) <= StallSlope && modelEnergyCoast > 0.4f*(sp-pv);
      if (stalled) { if (stallSince_ == 0U) stallSince_ = now; } else stallSince_ = 0U;
      if (stalled && static_cast<uint32_t>(now-stallSince_) >= StallDelayMs) stallRelax_ = fminf(1.0f, stallRelax_ + dtS/StallRampS);
      else if (!stalled) stallRelax_ = fmaxf(0.0f, stallRelax_ - dtS/StallDecayS);
      if (stallRelax_ > 0.0f) expectedCoast = fmaxf(slopeCoast, (1.0f - stallRelax_) * energyCoast);
    }
    // A learned profile (hint strength hs) already replaces the legacy assumption with data: the self-calibration only
    // covers the unlearned time, so it fades out as the hint takes over.
    if (smart_ && !launch) expectedCoast = fmaxf(expectedCoast, (1.0f - hs) * smartCoast(now, sp-pv, excessOnMs, holdEff));
    const float error = sp-pv;
    const float peak = pv+expectedCoast;
    lastPeak_ = peak;
    // The 10 s IIR slope jumps to 0.005..0.01 C/s at every 0.1 C probe step although PV is not moving, so with a coarse probe the
    // controller never became "stable", never reached Hold and kept the integral frozen below SP - 0.15 C for good (post-tune run of the
    // 0.1 C unit plant: 0.14 C under SP for three hours). Against the slow average a step is at most one probe step, a real climb of
    // 0.002 C/s is 0.3 C: the slope only counts when PV is also away from its slow average.
    const bool pvMoving = fabsf(pv-pvSlow_) > StableDevC;
    if (fabsf(error) <= 0.45f && (fabsf(slope_) <= 0.002f || (!pvMoving && fabsf(slope_) <= StableStepSlope))) {
      if (stableAt_ == 0U) stableAt_ = now;
      if (static_cast<uint32_t>(now-stableAt_) >= 60000U) phase_ = Phase::Hold;
    } else stableAt_ = 0U;
    if (phase_ == Phase::Hold && (error > 0.7f || error < -0.25f || (slope_ > 0.006f && pvMoving)))
      phase_ = Phase::Approach;
    if (phase_ != Phase::Hold) {
      if (error > 2.0f && peak < sp-1.0f) phase_ = Phase::FullHeat;
      else if (error > 0.8f && peak < sp-0.3f) phase_ = Phase::Approach;
      else phase_ = Phase::SoftLanding;
    }
    // HOLD is still a delayed 16 kW plant. Never bypass the same braking
    // envelope merely because the filtered PV was momentarily stationary.
    // The ceiling lets the predicted peak reach SP + CeilOverC (a measured plant is braked by its predicted peak; the PID closes the last
    // tenth); the boost request below tapers off earlier, so the PID, not the brake, finishes the approach.
    const float remaining = error-expectedCoast+(trusted ? (phase_ == Phase::Hold ? HoldCeilOverC : CeilOverC) : 0.0f);
    // Braking band: 0.5 C (legacy). A measured plant has a model error of a fraction of its full-power coast, and that error times the steep
    // fraction-per-degree of a narrow band is a limit cycle on a strong plant with a long dead time (G 0.1 C/s, 77 s: +-0.7 C every two
    // minutes), so the band grows with the full-power coast.
    float band = 0.5f;
    if (trusted) band = BandBaseC + BandPerCoast * modelGain * (lv.dhS + LagS);
    const float brakeFraction=clampFloat((remaining+0.05f)/band, 0.0f, 1.0f);
    // The predictive fraction refers to the rated bank. External authority
    // remains a separate final limit and never redefines physical 100%.
    float cap = 100.0f*brakeFraction;
    {
      // Hold-aware braking: the legacy law is a pure proportional cap that is unaware of the
      // duty needed just to stand still, so a plant with 33 % hold power can only balance at
      // error >= ~0.07 C (more for bigger hold). Brake the SURPLUS above the learned hold
      // instead; go below hold only when the predicted coast really overshoots SP.
      // (launch: the hold measured on this launch or stored in the plan, at full weight; else the learned hint, by its strength)
      const float wHold = lv.holdFull > 0.0f ? 1.0f : (trusted ? 0.0f : hs);
      const float holdCap = clampFloat(lv.holdFull > 0.0f ? lv.holdFull : holdEff, 0.0f, maxPower);
      if (wHold > 0.0f) {
        const float f = clampFloat((remaining+0.05f)/band, -1.0f, 1.0f);
        const float learnedCap = f >= 0.0f ? holdCap + (maxPower-holdCap)*f : holdCap*(1.0f+f);
        cap += wHold*(learnedCap - cap);
      }
    }
    // Near SP, a stationary heavy load can suddenly become light before the
    // remote sensor sees it. Allow only a bounded increment above established
    // maintenance duty until the changed slope is observed.
    if (error < 2.0f && !trusted) {   // a measured plant is braked by its predicted peak, not by a fixed 20 % cap
      const float legacyFloor = fmaxf(20.0f, holdModel+5.0f);
      float floorCap = legacyFloor;
      if (hs > 0.0f && hint_.gainPerSec > 0.0f) {
        // Learned model: hold + exactly the surplus that closes the remaining error with a
        // first-order approach (time constant 3x the learned delay, >= 90 s). It shrinks to
        // the hold duty as the error vanishes, so PV can actually reach SP without a late burst.
        const float tApproach = fmaxf(90.0f, 3.0f*hint_.delaySec);
        const float surplus = 100.0f*clampFloat(error, 0.0f, 2.0f)/(tApproach*hint_.gainPerSec);
        const float learned = clampFloat(holdEff + surplus + 3.0f, 0.0f, maxPower);
        floorCap = legacyFloor + hs*(learned - legacyFloor);
      }
      cap = fminf(cap, floorCap);
    }
    if (slopeEff > 0.0f && !trusted) {   // (a measured plant has its slope in the predicted peak)
      float rateCap = 100.0f*clampFloat(
          (error+0.1f)/(slopeEff*coastSeconds+0.1f), 0.0f, 1.0f);
      if (hs > 0.0f && hint_.gainPerSec > 0.0f) {
        // Model-based slope cancel: hold power, minus exactly the duty that removes the slope
        // in excess of what would reach SP in coastSeconds (dS / Kh). It equals hold on a
        // consistent approach and falls below it only as far as the physics requires, instead
        // of the legacy cut to ~0 % when PV is merely a tenth of a degree high.
        const float allowedSlope = fmaxf(0.0f, error) / fmaxf(30.0f, coastSeconds);
        const float learnedRate = clampFloat(clampFloat(holdEff, 0.0f, maxPower) +
            100.0f*(allowedSlope - slopeEff)/hint_.gainPerSec, 0.0f, maxPower);
        rateCap += hs*(learnedRate - rateCap);
      }
      cap = fminf(cap, rateCap);
    }
    cap = clampFloat(cap, 0.0f, maxPower);
    cap = fminf(cap, lastRequested_+30.0f);
    float floorPct = 0.0f;
    bool cutHold = false;
    boostNow_ = false;
    if (trusted) {
      // ---- Overshoot: the model under-braked. More braking from now on, and the stored plan is not trusted for the rest of the batch.
      if (pv > pvMaxAfterBoost_) pvMaxAfterBoost_ = pv;
      if (lastBoostAt_ != 0U && static_cast<uint32_t>(now-lastBoostAt_) < 300000U && pv > sp + OvershootCalC && !overshootSeen_) {
        overshootSeen_ = true;
        coastCal_ = clampFloat((calKnown_ ? coastCal_ : 1.0f) * OvershootCalUp, CalMin, CalMax); calKnown_ = true; planBad_ = true;
      }
      if (pv < sp) overshootSeen_ = false;
      // ---- Drift measurement: the first time the predicted peak comes within CutMarginC of SP while the heat in flight is large (once per
      // warm-up, only without a known calibration), the heater is cut to 0 and held there until PV has peaked: the rise after the cut / the
      // modelled heat in flight is the calibration of the model (the "drift").
      if (cutActive_) {
        cutHold = true;
        if (pv > cutPeak_) cutPeak_ = pv;
        const uint32_t el = static_cast<uint32_t>(now-cutAt_);
        const bool flat = windowSlope(now) <= CutFlatSlope;
        if (flat) { if (cutFlatAt_ == 0U) cutFlatAt_ = now; } else cutFlatAt_ = 0U;
        const uint32_t waitMs = static_cast<uint32_t>((lv.dhS + 2.0f*LagS) * 1000.0f);
        const uint32_t minWaitMs = waitMs > CutMinWaitMs ? waitMs : CutMinWaitMs;
        const bool settled = el >= minWaitMs && ((cutFlatAt_ != 0U && static_cast<uint32_t>(now-cutFlatAt_) >= 20000U) ||
                                                 pv <= cutPeak_ - 0.05f);
        if (disturbed_ || lastOn_) { cutActive_ = false; cutDone_ = true; cutHold = false; }   // contaminated: no calibration from this one
        else if (settled || el >= CutMaxMs || pv >= sp - 0.3f) {
          const float drift = cutPeak_ - cutPv_;
          if (cutPred1_ >= CutMinPredC && drift >= 0.05f && pv < sp - 0.3f) {
            coastCal_ = clampFloat(drift / cutPred1_, CalMin, CalMax); calKnown_ = true;
            cutDrift_ = drift; calHoldDone_ = false;
          }
          cutActive_ = false; cutDone_ = true; cutHold = false;
        }
      } else if (!cutDone_ && !calKnown_ && lv.informative && !disturbed_ && phase_ != Phase::Hold && pv < sp - BoostMinErrC && error <= CutMaxErrC &&
                 error - lv.gEst * excessNominalS < CutMarginC && lv.gEst * excessNominalS >= CutMinPredC) {
        cutActive_ = true; cutHold = true; cutAt_ = now; cutPv_ = pv; cutPeak_ = pv; cutFlatAt_ = 0U;
        cutPred1_ = lv.gEst * excessNominalS; cutIfS_ = lv.inFlightS; cutWinS_ = lv.windowS; cutG_ = lv.gEst;
        cutX_ = clampFloat(1.0f - (sp-pv)/12.0f, 0.5f, 1.0f);
      }
      if (cutHold) cap = 0.0f;
      // ---- Accelerate-then-brake: while the plant is measured, the predicted peak (not the PID's P band) sets the power, full power until
      // the heat in flight would carry PV to SP, then the braking fraction tapers it to the hold.
      else if (phase_ != Phase::Hold && pv < sp - BoostMinErrC) {
        // power the predicted peak allows while it is still BoostTargetC under SP, the same hold-aware law as the ceiling
        const float fB = clampFloat((remaining - (phase_ == Phase::Hold ? HoldCeilOverC : CeilOverC) - BoostTargetC + 0.05f) / band, -1.0f, 1.0f);
        const float holdB = clampFloat(lv.holdFull, 0.0f, maxPower);
        const float capB = fB >= 0.0f ? holdB + (maxPower - holdB) * fB : holdB * (1.0f + fB);
        floorPct = fminf(cap, capB); boostNow_ = true; lastBoostAt_ = now;
      }
    }
    lastCap_ = cap;
    // ---- Plan of this warm-up, for the next batch: gain + delay + drift calibration + hold, once PV has stood still near SP for 90 s.
    if (!planOffered_ && trusted && lv.informative && onset_ && holdValid_ && fabsf(error) <= PlanNearSpC && !disturbed_ &&
        pvMaxAfterBoost_ <= sp + PlanMaxOverC && windowSlope(now) <= FlatSlope && windowSlope(now) >= -FlatSlope) {
      if (holdSince_ == 0U) holdSince_ = now;
      else if (static_cast<uint32_t>(now-holdSince_) >= 90000U) {
        planOffered_ = true;
        MayapThermal::LaunchPlan out;
        out.valid = true; out.gain = clampFloat(lv.gEst, 0.0005f, 0.5f);
        out.delaySec = clampFloat(lv.dhS, 0.0f, DelayUnknownS);   // the delay the arrival kernel used on this launch
        out.coastCal = calKnown_ ? coastCal_ : DefaultPlanCal;
        out.calMeasured = calKnown_;
        out.holdPct = holdLive_ > 0.0f ? clampFloat(holdLive_, 0.0f, 100.0f) : 0.0f;
        const bool same = plan_.valid && fabsf(out.gain-plan_.gain) <= 0.10f*plan_.gain &&
                          fabsf(out.delaySec-plan_.delaySec) <= 10.0f && fabsf(out.coastCal-plan_.coastCal) <= 0.15f &&
                          out.calMeasured == plan_.calMeasured && fabsf(out.holdPct-plan_.holdPct) <= 3.0f;
        if (!same) { planOut_ = out; planReady_ = true; }
      }
    } else holdSince_ = 0U;
    const bool freezeIntegral = phase_ != Phase::Hold &&
        !(error > 0.15f && slope_ <= 0.001f && peak < sp-0.2f);
    return {cap, freezeIntegral, phase_, peak, floorPct};
  }
  void requested(float power) { lastRequested_ = fmaxf(0.0f, power); }
  bool sampleStale(uint32_t now) const {
    return historyGap_ ||
        (initialized_ && static_cast<uint32_t>(now-lastSampleAt_) > 6000U);
  }
  Phase phase() const { return phase_; }
  float predictedPeak() const { return lastPeak_; }
  float lastCeiling() const { return lastCap_; }
  // What the launch measured on this oven (0 = not measured yet), for the HMI PID monitor: gain [degC/s at 100 %], delay [s], hold [% duty].
  float launchGain() const { return gainInfo_ ? gainEst_ : 0.0f; }
  float launchDelaySec() const { return onset_ ? lastDhS_ : 0.0f; }
  float launchHoldPct() const { return holdValid_ && holdLive_ > 0.0f ? holdLive_ : 0.0f; }
  float slope() const { return slope_; }
  // read-only telemetry (host simulation / diagnostics)
  float learnedHold() const { return holdPower_; }
  bool riseSeen() const { return firstRiseAt_ != 0U; }
  bool coastClear() const { return coastCleared_; }
 private:
  static constexpr uint8_t Buckets = 120U;
  static constexpr float MinimumCapacity = 180000.0f;
  // Design envelope of the Smart startup. PriorGain: the heating rate (degC/s at 100 % bank) of the strongest plant the
  // simulation domain allows (16 kW x effectiveness 2.0 on 180 kJ/degC = 0.178) plus 10 %. It applies ONLY until the first
  // response has been seen; afterwards the measured gain replaces it. Both only add braking, they never add heat.
  static constexpr float PriorGain = 0.196f;
  static constexpr float LegacyGain = 16.0f * 1.35f / 180000.0f * 1000.0f;   // 0.12 degC per ON-second, as in decide()
  static constexpr uint32_t PriorWindowMs = 150000U;
  static constexpr float PriorMinError = 3.5f;
  static constexpr float GainMargin = 1.15f;     // slope lags the heat and the loss subtracts from it: bias the estimate up
  static constexpr float GainCeiling = 0.5f;     // sanity bound, degC/s
  static constexpr float StableDevC = 0.12f;      // PV within this of its slow average = not moving (one 0.1 C probe step)
  static constexpr float StableStepSlope = 0.012f; // ... and the IIR slope below this = a probe step, not a climb
  static constexpr float SlowPvTauSec = 150.0f;  // time constant of the slow PV average (flatness reference)
  static constexpr float SlowHoldRiseC = 0.03f;   // PV - slow average above this = rising (slope ~ 0.0002 C/s): not flat
  static constexpr float SlowHoldDriftC = 0.02f;  // slow average moved less than this over one SlowHoldFlatMs window = flat
  static constexpr float SlowHoldFallC = 0.06f;   // ... below minus this = falling
  static constexpr uint32_t SlowHoldFlatMs = 360000U;   // flat for this long = the heat in flight is stationary
  static constexpr float SlowHoldMeasuredFrac = 0.95f;  // share of the measured equilibrium duty taken as the hold
  static constexpr float SlowHoldPctPerMin = 0.2f;      // slow integrator that closes the last offset
  static constexpr float SlowHoldCeilFactor = 1.5f;     // ... which stops at  1.5 x the measured flat-PV duty + SlowHoldCeilPct
  static constexpr float SlowHoldCeilPct = 2.0f;
  static constexpr float FirstRiseC = 0.25f;            // PV rise since the first heat that counts as 'the heat has surfaced' for a weak oven
  static constexpr uint32_t GainHistStepMs = 10000U;    // ON-time / PV snapshots for the delayed accounting
  static constexpr uint32_t GainHistLen = 80U;          // ... 800 s of history (gain lag, drift and the hold window)
  // Launch (accelerate-then-brake on a measured plant). Everything below is a design number of the launch model, in seconds, degC or fractions.
  static constexpr float PvFastTauSec = 15.0f;          // PV smoothing for the windowed slope / gain (probe quantization)
  static constexpr float LagS = 40.0f;                  // time constant of the heat arrival after the delay
  static constexpr float OnsetBackS = 20.0f;            // the heat starts to arrive about this long before the rise is seen. A strong plant shows it at once, a
                                                         // weak one ~35 s late: a late delay only adds caution (the arrived heat is under-counted, the gain
                                                         // over-counted); an early one is the unsafe side
  static constexpr float DelayMinS = 15.0f;
  static constexpr float DelayUnknownS = 270.0f;        // delay assumed until the rise has been seen (the longest the envelope allows)
  static constexpr float OnsetMinElapsedS = 15.0f;
  static constexpr float LambdaMinS = 45.0f;            // the baseline cooling slope needs this long a fall to be read
  static constexpr float LambdaNoiseC = 0.05f;          // PV noise (probe quantization) over the span of the cooling-slope measurement
  static constexpr float RiseFromMinC = 0.12f;          // PV this far above its minimum since the first heat, and still rising = the heat has surfaced
  static constexpr float OnsetSlope = 0.0004f;
  static constexpr float ProbeGain = 0.2f;              // degC/s at 100 %: ceiling of the design envelope until the launch has evidence
  static constexpr float SigmaBaseC = 0.06f;            // PV noise (filter + 0.1 C probe) in the gain measurement
  static constexpr float SigmaLossFrac = 0.25f;         // uncertainty of the loss compensation
  static constexpr float LossRf = 1.15f;                // the loss grows while PV climbs: factor on the baseline cooling slope
  static constexpr float BoundMinPhiS = 6.0f;           // arrived ON-seconds before the gain ceiling is used
  static constexpr float InfoPhiS = 15.0f;              // ... before the gain counts as measured ...
  static constexpr float InfoSigmas = 3.0f;             // ... and the response must be this many sigmas
  static constexpr float HeavyShare = 0.5f;             // the gain is only measured while this share of the last 120 s was heating
  static constexpr float MatureS = 390.0f;              // ON-time older than this has surely arrived (the longest delay + 3 lags of the design envelope)
  static constexpr uint32_t MatureBins = 39U;
  static constexpr float GainAnchorS = 780.0f;          // the gain window is anchored at the first heat up to this long (the history ring is 800 s)
  static constexpr float GainWinS = 400.0f;             // ... then, only if the gain is still unmeasured, a sliding window of this length
  static constexpr float HoldWinS = 360.0f;
  static constexpr float IdleInFlightS = 3.0f;          // idle chamber: less heat than this (ON-seconds) still on its way
  static constexpr float IdleFallSlope = 0.0003f;       // ... and PV falling faster than this
  static constexpr float HoldMaxErrC = 4.0f;            // the hold is read this far under SP too (the loss there is a little smaller: a lower bound)
  static constexpr float HoldFlatC = 0.06f;             // PV moved less than this over the hold window = standing still
  static constexpr float HoldRangeC = 0.8f;             // PV range over the hold window above which the balance is not read (a step, not the loss)
  static constexpr float HoldMaxPct = 60.0f;
  static constexpr float HoldRisePerSnapshot = 0.5f;    // hold credit may rise this many points per 10 s (more credit = less braking)
  static constexpr float GainInfoMargin = 1.25f;        // on the measured gain
  static constexpr float LiveFall = 0.9f;              // the gain ceiling may fall by 3 % per 10 s snapshot, it rises at once
  static constexpr float PlanReserve = 1.6f;            // prior gain not yet confirmed by this launch (a different number of egg trolleys changes the gain)
  static constexpr float CoastKappa = 1.2f;             // reserve on the modelled coast until the drift has been measured
  static constexpr float CalMargin = 1.15f;             // on the measured drift ratio
  static constexpr float CalMin = 0.5f;
  static constexpr float CalMax = 3.0f;
  static constexpr float ReserveMin = 1.05f;
  static constexpr float DefaultPlanCal = 1.4f;         // plan saved without a measured drift: conservative
  static constexpr float HoldPlanCredit = 0.8f;         // share of the plan's hold credited before this launch has measured its own
  static constexpr float HoldMeasuredCredit = 1.0f;
  static constexpr float KpLoopK = 70.0f;               // Kp x gain x (delay + lag) bound (% x degC/s per 100 % x s / 100): beyond it the loop rings
  static constexpr float TiPerDelay = 4.0f;             // integral time >= this x (delay + lag)
  static constexpr float HoldFfShare = 0.95f;           // share of the measured hold fed forward to the PID
  static constexpr float BaselineCreditMaxPct = 30.0f;  // hold credit from the cooling seen before the heat surfaced (a floor: the loss only grows with PV)
  static constexpr float CeilOverC = 0.1f;              // the ceiling lets the predicted peak reach SP + this
  static constexpr float HoldCeilOverC = 0.3f;          // ... in HOLD, SP + this (the PID owns the last tenths)
  static constexpr float BoostTargetC = 0.4f;           // the boost request tapers off when the predicted peak is this far under SP
  static constexpr float BandBaseC = 0.2f;              // braking band of a measured plant = this + BandPerCoast x its full-power coast
  static constexpr float BandPerCoast = 0.03f;
  static constexpr float BoostMinErrC = 0.5f;           // below this the PID (with the hold feed-forward) takes over; the brake stays a ceiling
  static constexpr float CutMarginC = 1.0f;             // cut when the predicted peak is this far under SP (first warm-up only)
  static constexpr float CutMaxErrC = 3.0f;             // the drift is measured near SP, never from far below it
  static constexpr float CutMinPredC = 0.4f;            // ... and the heat in flight is at least this much
  static constexpr uint32_t CutMinWaitMs = 60000U;
  static constexpr uint32_t CutMaxMs = 420000U;
  static constexpr float CutFlatSlope = 0.0006f;
  static constexpr float FlatSlope = 0.0006f;           // PV counts as not moving below this 60 s slope
  static constexpr float StallMinErrC = 0.3f;           // stall backstop: PV this far under SP, not moving ...
  static constexpr float StallSlope = 0.0003f;
  static constexpr uint32_t StallDelayMs = 400000U;     // ... for longer than the longest delay + lag of the envelope
  static constexpr float StallRampS = 360.0f;
  static constexpr float StallDecayS = 600.0f;
  static constexpr float PlanBadMinErrC = 1.0f;         // the stored plan is dropped when, this far under SP ...
  static constexpr uint32_t PlanBadMinOnMs = 30000U;    // ... with this much heat given, nothing rises after twice its delay
  static constexpr float PlanNearSpC = 0.5f;
  static constexpr float SetpointRelaunchC = 0.5f;      // a set point raised by more than this restarts the launch (the plan stays as the prior)
  static constexpr float WarmStartErrC = 0.6f;          // the first heat with PV this close to SP is a restart in the hold, not a warm-up
  static constexpr float OvershootCalC = 0.15f;         // PV this far above SP after a boost: the model under-braked
  static constexpr float OvershootCalUp = 1.5f;
  static constexpr float PlanMaxOverC = 0.8f;           // a plan is not saved from a warm-up that overshot more than this
  static constexpr float SlowHoldCutSpanC = 0.25f;      // overshoot above SP + 0.05 C at which the hold is lowered at the full rate
  // ---- Launch model --------------------------------------------------------------------------------------------------------------------
  // Heat of an ON-second reaches PV after the delay dh, then as a first-order lag (time constant LagS): the part still on its way at age a is
  // 1 for a <= dh and exp(-(a-dh)/LagS) after. The delay is the one MEASURED on this launch (the PV turning upward, less the time the rise
  // needs to show), else the plan's, else the longest the design envelope allows. ON-time is metered per 10 s bin from the actual SSR state.
  static float kernelW(float ageS, float dhS) { return ageS <= dhS ? 1.0f : expf(-(ageS - dhS) * (1.0f / LagS)); }
  float delayHatS(const Prior &prior) const {
    // (a weak response shows late: a measured delay beyond the design envelope is a weak plant, not a longer delay)
    if (onset_) return clampFloat(onsetElapsedS_ - OnsetBackS, DelayMinS, DelayUnknownS);
    if (prior.valid) return clampFloat(prior.delaySec, DelayMinS, DelayUnknownS);
    return DelayUnknownS;
  }
  // Sums over the bins older than shiftS: inFlight = sum on * w(age - shiftS), window = sum duration * w, onSec = sum on.
  void sumBins(uint32_t now, float dhS, float shiftS, float &inFlight, float &window, float &onSec) const {
    inFlight = window = onSec = 0.0f;
    if (!gainHistInit_ || firstHeatAt_ == 0U) return;
    const float frac = static_cast<float>(static_cast<uint32_t>(now - gainHistBid_*GainHistStepMs)) * 0.001f;
    const float sinceHeat = static_cast<float>(static_cast<uint32_t>(now - firstHeatAt_)) * 0.001f;
    {
      const float onPart = static_cast<float>(static_cast<uint32_t>(onTotalMs_ - gainHist_[gainHistBid_ % GainHistLen])) * 0.001f;
      const float age = 0.5f * frac;
      if (age >= shiftS) {
        const float w = kernelW(age - shiftS, dhS);
        inFlight += onPart * w; onSec += onPart; window += fminf(frac, sinceHeat) * w;
      }
    }
    for (uint32_t k = 0; k + 1U < GainHistLen && k < gainHistBid_; ++k) {
      const float age = frac + 10.0f * static_cast<float>(k) + 5.0f;
      const float sinceBin = sinceHeat - frac - 10.0f * static_cast<float>(k);    // heating time at the start of this bin's end boundary
      if (sinceBin <= 0.0f) break;
      if (age < shiftS) continue;
      const uint32_t j = gainHistBid_ - k;
      const float on = static_cast<float>(static_cast<uint32_t>(gainHist_[j % GainHistLen] - gainHist_[(j-1U) % GainHistLen])) * 0.001f;
      const float w = kernelW(age - shiftS, dhS);
      inFlight += on * w; onSec += on; window += fminf(10.0f, sinceBin) * w;
    }
  }
  // The PV turning upward after the first heat = the heat has surfaced. Less sensitive than the legacy first-rise (it follows the minimum,
  // so a cooling baseline does not hide the response) and it freezes the baseline cooling slope used to compensate the loss.
  void trackOnset(uint32_t now) {
    if (firstHeatAt_ == 0U || !pvStartSet_ || warm_ || onset_) return;
    const float elapsed = static_cast<float>(static_cast<uint32_t>(now - firstHeatAt_)) * 0.001f;
    // Baseline cooling slope of the time before any heat can have arrived: the PV falls at (pvStart - pvMin) / tMin until the minimum.
    if (!pvMinSet_ || pvFast_ < pvMin_) { pvMin_ = pvFast_; pvMinAt_ = now; pvMinSet_ = true; }
    const float tMin = static_cast<float>(static_cast<uint32_t>(pvMinAt_ - firstHeatAt_)) * 0.001f;
    lambda0_ = tMin >= LambdaMinS ? fmaxf(0.0f, (pvStart_ - pvMin_) / tMin) : 0.0f;
    lambdaT_ = tMin;
    // PV with the baseline cooling added back; its rise above its own minimum is the response of the heat. Either the PV itself has turned
    // upward by RiseFromMinC, or it has stopped falling and the compensated PV has risen by twice that (a heater weaker than the loss it
    // fights shows its heat only as a flattening of the fall).
    const float pvc = pvFast_ + lambda0_ * elapsed;
    if (!pvcMinSet_ || pvc < pvcMin_) { pvcMin_ = pvc; pvcMinSet_ = true; }
    const float turned = pvFast_ - pvMin_;
    const bool rising = turned >= RiseFromMinC && windowSlope(now) >= OnsetSlope;
    const bool flattened = turned >= 0.5f * RiseFromMinC && pvc - pvcMin_ >= 2.0f * RiseFromMinC && windowSlope(now) >= OnsetSlope - lambda0_;
    if (elapsed >= OnsetMinElapsedS && (rising || flattened)) {
      onset_ = true; onsetElapsedS_ = elapsed;
    }
  }
  LaunchView launchView(uint32_t now, const Prior &pr) const {
    LaunchView v;
    v.dhS = delayHatS(pr);
    float ifS, winS, onS;
    sumBins(now, v.dhS, 0.0f, ifS, winS, onS);
    v.inFlightS = ifS; v.windowS = winS;
    const float priorGain = pr.valid ? pr.gain * PlanReserve : ProbeGain;
    v.gEst = gainInfo_ ? gainEst_ : 0.0f;
    v.informative = gainInfo_;
    // measured: the gain with a margin for its error (the ceiling is for the time before it was measured: it is not updated while the heater is throttled)
    v.gUse = gainInfo_ ? fminf(gainLive_, GainInfoMargin * gainEst_) : (gainLive_ > 0.0f ? fminf(priorGain, gainLive_) : priorGain);
    v.kappa = calKnown_ ? clampFloat(CalMargin * coastCal_, ReserveMin, CalMax * CalMargin) : CoastKappa;
    float hc = 0.0f, hf = 0.0f;
    if (holdValid_ && holdLive_ > 0.0f) { hc = HoldMeasuredCredit * holdLive_; hf = holdLive_; }
    else if (pr.valid && pr.holdPct > 0.0f) { hc = HoldPlanCredit * pr.holdPct; hf = pr.holdPct; }
    else if (gainInfo_ && gainEst_ > 0.0f) hc = clampFloat(100.0f * lambda0_ / gainEst_, 0.0f, BaselineCreditMaxPct);   // loss seen before the heat arrived: a floor of the hold
    v.holdCred = hc; v.holdFull = hf;
    return v;
  }
  // Every GainHistStepMs: snapshot the ON-time total and the smoothed PV, then re-measure the heater gain and the hold over the sliding window.
  void snapshotRing(uint32_t now) {
    const uint32_t bid = now / GainHistStepMs;
    if (!gainHistInit_) {
      for (uint32_t k = 0; k < GainHistLen; ++k) { gainHist_[k] = onTotalMs_; pvHist_[k] = pvFast_; }
      gainHistBid_ = bid; gainHistInit_ = true;
      return;
    }
    if (bid == gainHistBid_) return;
    uint32_t steps = bid - gainHistBid_;
    if (steps > GainHistLen) steps = GainHistLen;
    // (a gap between two snapshots spreads the ON-time of the gap evenly over the missed bins)
    const uint32_t onBefore = gainHist_[gainHistBid_ % GainHistLen], onGap = static_cast<uint32_t>(onTotalMs_ - onBefore);
    for (uint32_t k = 1; k <= steps; ++k) {
      gainHist_[(gainHistBid_ + k) % GainHistLen] = onBefore + static_cast<uint32_t>(static_cast<uint64_t>(onGap) * k / steps);
      pvHist_[(gainHistBid_ + k) % GainHistLen] = pvFast_;
    }
    gainHistBid_ = bid;
    if (firstHeatAt_ == 0U || !pvStartSet_ || disturbed_ || steps > 3U) return;
    launchEstimate(now);
  }
  // Gain: C of PV rise (plus the loss over the same time, from the cooling slope seen before the heat surfaced) per ARRIVED ON-second.
  //   resp = dPV + loss;  gain = resp / arrived;  ceiling = (resp + 2 sigma) / arrived  (a bound even when nothing has been seen).
  // Hold: the duty whose heat arrived over the window less what PV gained, i.e. the loss, once PV is nearly standing still.
  void launchEstimate(uint32_t now) {
    const float elapsed = static_cast<float>(static_cast<uint32_t>(now - firstHeatAt_)) * 0.001f;
    if (elapsed < 20.0f) return;
    const float dh = lastDhS_;
    // A restart in the hold has no response to measure the gain from, but the hold is still read from the flat PV (the plan's hold is only a prior).
    if (warm_) { launchHold(now, elapsed, dh); return; }
    float if0, win0, on0;
    sumBins(now, dh, 0.0f, if0, win0, on0);
    // The gain is measured over the launch (anchored at the first heat, up to the 800 s ring). After that it is kept; only a launch that has
    // not measured it yet (a slow plant) goes on with a sliding window.
    const bool anchored = elapsed <= GainAnchorS;
    if (!anchored && gainInfo_) { launchHold(now, elapsed, dh); return; }
    float W = anchored ? elapsed : GainWinS;
    const uint32_t nW = static_cast<uint32_t>(W * 0.1f);
    if (nW < 2U || nW >= GainHistLen - 1U || nW > gainHistBid_) return;
    W = static_cast<float>(nW) * 10.0f;
    float phi, dPv;
    if (anchored) { phi = on0 - if0; dPv = pvFast_ - pvStart_; }
    else {
      float ifW, winW, onW;
      sumBins(now, dh, W, ifW, winW, onW);
      phi = ifW + (on0 - onW) - if0;
      dPv = pvFast_ - pvHist_[(gainHistBid_ - nW) % GainHistLen];
    }
    const float loss = lambda0_ * LossRf * W;
    const float sigma = SigmaBaseC + SigmaLossFrac * loss + (LambdaNoiseC / fmaxf(lambdaT_, LambdaMinS)) * W;
    const float resp = fmaxf(0.0f, dPv + loss);
    // Gain ceiling: valid for any window while the launch has not measured the gain (what has not been seen bounds it). Once measured, the
    // gain is only re-measured while the heater is really heating (>= HeavyShare of the last 120 s and of the window): over a cut or a
    // hold the balance would measure the loss, not the heater.
    uint32_t recentOn = 0;
    for (uint8_t i = 0; i < 60U; ++i) recentOn += onMs_[(bucket_+Buckets-i)%Buckets];
    const bool heating = recentOn >= 60000U * HeavyShare;
    // The measured delay can only be late (a weak response shows late), which makes the arrived heat too small and the gain too big. A second
    // ceiling does not depend on it: the ON-time older than MatureS has surely arrived (the longest delay + lag of the design envelope), and
    // everything PV has gained since the first heat, plus the loss, is at most the gain times that ON-time plus what is still arriving.
    float maturedCeil = INFINITY;
    if (anchored && elapsed > MatureS && gainHistBid_ >= MatureBins) {
      const float onM = static_cast<float>(static_cast<uint32_t>(gainHist_[(gainHistBid_ - MatureBins) % GainHistLen])) * 0.001f;
      if (onM >= BoundMinPhiS) maturedCeil = (fmaxf(0.0f, pvFast_ - pvStart_ + loss) + 2.0f * sigma) / onM;
    }
    if (phi >= BoundMinPhiS && (!gainInfo_ || heating)) {
      const float ceiling = fminf((resp + 2.0f * sigma) / phi, maturedCeil);
      gainLive_ = gainLive_ <= 0.0f ? ceiling : fmaxf(ceiling, gainLive_ * LiveFall);
      if (phi >= InfoPhiS && heating && resp >= InfoSigmas * sigma) { gainEst_ = fminf(resp / phi, maturedCeil); gainInfo_ = true; }
    } else if (isfinite(maturedCeil) && !gainInfo_) {
      gainLive_ = gainLive_ <= 0.0f ? maturedCeil : fmaxf(fminf(gainLive_, maturedCeil), gainLive_ * LiveFall);
    }
    launchHold(now, elapsed, dh);
  }
  void launchHold(uint32_t now, float elapsed, float dh) {
    float if0, win0, on0;
    sumBins(now, dh, 0.0f, if0, win0, on0);
    // Hold from the cooling of an idle chamber: no heat on its way and PV falling, the fall IS the loss (after a cut, once the peak has passed).
    // It needs only the measured gain, and is available minutes before PV stands still. It reads the loss at the current PV, a little under the
    // loss at SP: a lower bound, which only means a little more braking. It is latched (a later, busier window must not lower it).
    if (gainInfo_ && gainEst_ > 0.0f && if0 < IdleInFlightS && lastOn_ == false && gainHistBid_ >= 14U) {
      const float slope120 = (pvFast_ - pvHist_[(gainHistBid_ - 12U) % GainHistLen]) / 120.0f;
      if (slope120 <= -IdleFallSlope && slope120 >= -0.5f * gainEst_) {
        const float h = clampFloat(100.0f * (-slope120) / gainEst_, 0.0f, HoldMaxPct);
        if (holdIdle_ < 0.0f) holdIdle_ = h;
        else {
          const float step = 0.3f * (h - holdIdle_);
          holdIdle_ += step > HoldRisePerSnapshot ? HoldRisePerSnapshot : step;
        }
      }
    }
    // Hold with PV standing still (within the noise): the duty whose heat ARRIVED over the last HoldWinS IS the loss, no gain needed. A step in
    // PV (door, vent) makes the range large and the window is not read.
    const float err = lastSp_ - pvFast_;
    if (err <= HoldMaxErrC && err >= -0.3f && elapsed >= HoldWinS) {
      const uint32_t nH = static_cast<uint32_t>(HoldWinS * 0.1f);
      if (nH < GainHistLen - 1U && nH <= gainHistBid_) {
        float ifH, winH, onH;
        sumBins(now, dh, HoldWinS, ifH, winH, onH);
        const float phiH = ifH + (on0 - onH) - if0;
        const float dPvH = pvFast_ - pvHist_[(gainHistBid_ - nH) % GainHistLen];
        float lo = pvFast_, hi = pvFast_;
        for (uint32_t k = 0; k <= nH; ++k) { const float v = pvHist_[(gainHistBid_ - k) % GainHistLen]; if (v < lo) lo = v; if (v > hi) hi = v; }
        if (hi - lo <= HoldRangeC && fabsf(dPvH) <= HoldFlatC && phiH <= 0.6f * HoldWinS) {
          const float h = clampFloat(100.0f * phiH / HoldWinS, 0.0f, HoldMaxPct);
          if (holdFlat_ < 0.0f) holdFlat_ = h;
          else {
            // More hold credit means less braking: it rises slowly, it may fall at once.
            const float step = 0.3f * (h - holdFlat_);
            holdFlat_ += step > HoldRisePerSnapshot ? HoldRisePerSnapshot : step;
          }
        }
      }
    }
    holdLive_ = holdFlat_ >= 0.0f ? holdFlat_ : holdIdle_;
    holdValid_ = holdLive_ >= 0.0f;
    // The drift was measured against a prediction without any hold credit. Re-express it against the prediction WITH the credit that every later
    // launch (and this one from now on) uses, otherwise the credit would be counted twice.
    if (calKnown_ && !calHoldDone_ && holdValid_ && holdLive_ > 0.0f && cutDrift_ > 0.0f && cutG_ > 0.0f && cutIfS_ > 0.0f) {
      const float credit = HoldMeasuredCredit * holdLive_ * cutX_;
      const float excess = fmaxf(cutIfS_ - credit * 0.01f * cutWinS_, 0.25f * cutIfS_);
      coastCal_ = clampFloat(cutDrift_ / (cutG_ * excess), CalMin, CalMax);
      calHoldDone_ = true;
    }
  }
  // PV slope over the last 60 s from the smoothed ring (immune to the 0.1 C probe steps that make the 10 s IIR slope spike).
  float windowSlope(uint32_t now) const {
    if (!gainHistInit_ || gainHistBid_ < 6U) return slope_;
    const uint32_t oldBid = gainHistBid_ - 6U;
    const float dtS = static_cast<float>(now) * 0.001f - static_cast<float>(oldBid) * 10.0f;
    if (dtS < 30.0f) return slope_;
    return (pvFast_ - pvHist_[oldBid % GainHistLen]) / dtS;
  }
  uint32_t recentOnMsOf(uint32_t horizonMs) const {
    uint8_t n = static_cast<uint8_t>(std::min<uint32_t>(horizonMs / 2000U, Buckets));
    uint32_t sum = 0;
    for (uint8_t i = 0; i < n; ++i) sum += onMs_[(bucket_ + Buckets - i) % Buckets];
    return sum;
  }
  // Heater gain under (nearly) constant full power: slope / duty. The running maximum is kept: a lagging slope
  // under-reads (unsafe direction), a falling duty over-reads (safe direction).
  void observeGain(uint32_t now) {
    if (firstRiseAt_ == 0U || static_cast<uint32_t>(now - firstRiseAt_) < 20000U) return;
    uint32_t on20 = 0;
    for (uint8_t i = 1; i <= 10; ++i) on20 += onMs_[(bucket_ + Buckets - i) % Buckets];   // 10 completed 2 s buckets
    const float duty = static_cast<float>(on20) / 20000.0f;
    if (duty < 0.85f || !(slope_ > 0.01f)) return;
    const float est = fminf(slope_ / duty, GainCeiling);
    if (est > khObs_) khObs_ = est;
  }
  // Heat still to arrive: ON-time inside the observed apparent delay times the gain (prior until a response is seen).
  float smartCoast(uint32_t now, float error, float legacyExcessOnMs, float holdEff) const {
    if (firstRiseAt_ == 0U) {
      // Unobserved start. Everything delivered more than PriorWindow ago has surfaced in the envelope (dead time <= 120 s,
      // heater lag <= 30 s); a plant that still shows nothing is slow or was barely heated: the legacy model governs.
      if (firstHeatAt_ != 0U && static_cast<uint32_t>(now - firstHeatAt_) >= PriorWindowMs) return 0.0f;
      // The legacy model is wrong by at most PriorGain/0.12 = 1.63x. Below PriorMinError that error cannot reach the
      // High alarm margin, so the legacy estimate is left alone (no change for small start errors).
      if (error < PriorMinError) return 0.0f;
      return legacyExcessOnMs * PriorGain * 0.001f;
    }
    // Only a heater STRONGER than the legacy 35 % reserve (0.12 degC/s) is under-predicted by the legacy energy model;
    // for every other plant the legacy estimate stays the single authority (no behaviour change, no new conservatism).
    if (!(khObs_ * GainMargin > LegacyGain)) return 0.0f;
    const float delayMs = clampFloat(static_cast<float>(firstRiseAt_ - firstHeatAt_) + 12000.0f, 20000.0f, 240000.0f);
    const uint32_t horizon = static_cast<uint32_t>(delayMs);
    const float windowMs = static_cast<float>(std::min<uint32_t>(horizon, static_cast<uint32_t>(now - energyWindowStartedAt_)));
    const float excess = fmaxf(0.0f, static_cast<float>(recentOnMsOf(horizon)) - holdEff * 0.01f * windowMs);
    return excess * khObs_ * GainMargin * 0.001f;
  }
  bool smart_ = (MAYAP_SMART_STARTUP != 0);
  float khObs_ = 0;
  uint32_t onMs_[Buckets]{};
  uint32_t observedAt_ = 0, bucketAt_ = 0;
  uint32_t firstHeatAt_ = 0, firstRiseAt_ = 0, lastHeatOffAt_ = 0;
  uint32_t energyWindowStartedAt_ = 0, stableAt_ = 0, lastSampleAt_ = 0;
  uint8_t bucket_ = 0;
  bool initialized_ = false, observed_ = false, lastOn_ = false, coastCleared_ = false;
  bool historyGap_ = false;
  float lastPv_ = 0, lastSp_ = 0, slope_ = 0, pvSlow_ = 0;
  float holdMeasured_ = -1.0f;   // duty measured while PV was flat below SP (-1 = none yet)
  uint32_t flatMs_ = 0;
  uint32_t onTotalMs_ = 0U;      // heater ON-time since the first heat (measured gain)
  float pvStart_ = 0.0f;
  bool pvStartSet_ = false;
  bool warm_ = false;   // the heat started with PV already at the set point and a plan is known (a restart in the hold): there is no response to measure, the plan's model governs
  float holdLive_ = -1.0f, holdIdle_ = -1.0f, holdFlat_ = -1.0f;   // hold (% duty): in use / from an idle chamber's cooling / from a standing-still PV, -1 = none yet
  float pvFast_ = 0.0f, gainLive_ = 0.0f, gainEst_ = 0.0f, coastCal_ = 1.0f;   // C/s at 100 %: ceiling / measured gain, drift calibration
  bool gainInfo_ = false, holdValid_ = false, onset_ = false, pvMinSet_ = false, pvcMinSet_ = false;
  float onsetElapsedS_ = 0.0f, lastGUseS_ = 0.2f, pvMin_ = 0.0f, pvcMin_ = 0.0f, lambda0_ = 0.0f, lambdaT_ = 0.0f, lastDhS_ = DelayUnknownS;
  uint32_t pvMinAt_ = 0U, onsetSeen_ = 0U;
  bool calKnown_ = false, planBad_ = false, disturbed_ = false;
  bool cutActive_ = false, cutDone_ = false, planOffered_ = false, planReady_ = false, overshootSeen_ = false;
  bool boostNow_ = false, trustedNow_ = false;
  uint32_t cutAt_ = 0U, cutFlatAt_ = 0U, holdSince_ = 0U, lastBoostAt_ = 0U;
  float cutPv_ = 0.0f, cutPred1_ = 0.0f, cutPeak_ = 0.0f, pvMaxAfterBoost_ = 0.0f;
  float holdFf_ = 0.0f, tiMin_ = 0.0f, kpMax_ = 0.0f, stallRelax_ = 0.0f;
  uint32_t stallSince_ = 0U;
  float cutIfS_ = 0.0f, cutWinS_ = 0.0f, cutG_ = 0.0f, cutX_ = 1.0f, cutDrift_ = 0.0f;
  bool calHoldDone_ = false;
  MayapThermal::LaunchPlan plan_{}, planOut_{}, retained_{};   // stored plan (a prior from outside), the launch's own knowledge kept across reset()
  float pvHist_[80]{};
  uint32_t gainHist_[80]{};
  uint32_t gainHistBid_ = 0U;
  bool gainHistInit_ = false;
  uint32_t flatMarkAt_ = 0U;     // start of the current hold-measurement window (0 = none)
  float flatMarkPv_ = 0.0f;      // slow PV average at the start of that window
  float lastRequested_ = 0, holdPower_ = 0, lastPeak_ = 0, lastCap_ = 0;
  MayapThermal::StartupHint hint_{};
  Phase phase_ = Phase::FullHeat;
};

// These phases/reasons are service diagnostics, not changes to public state codes.
enum class AutoTunePhase : uint8_t {
  Idle, Preheat, Heating, Cooling, Validating, Success, Failed,
  // Smart AutoTune V1 (appended: the legacy numbering above is unchanged)
  Baseline, Excite, Coast, Excite2, Approach, NearSp, Settle, Generate
};
enum class AutoTuneReason : uint8_t {
  None, SafetyAbort, SensorAbort, ModeAbort, PreheatTimeout, PhaseTimeout,
  TotalTimeout, NonRepeatable, AmplitudeTooSmall, PeriodTooSmall,
  InvalidKu, InvalidGains, SaveFailed, Success,
  // Smart AutoTune V1 (appended)
  NoHeadroom, BaselineUnstable, NoResponse, ModelInvalid, PowerLimited, ApproachTimeout,
  RelayFailed, CandidateInvalid, ValidationFailed
};
inline const char *autoTunePhaseName(AutoTunePhase phase) {
  switch(phase) {
    case AutoTunePhase::Idle:return "IDLE";case AutoTunePhase::Preheat:return "PREHEAT";
    case AutoTunePhase::Heating:return "HEATING";case AutoTunePhase::Cooling:return "COOLING";
    case AutoTunePhase::Validating:return "VALIDATING";case AutoTunePhase::Success:return "SUCCESS";
    case AutoTunePhase::Failed:return "FAILED";
    case AutoTunePhase::Baseline:return "BASELINE";case AutoTunePhase::Excite:return "EXCITE";
    case AutoTunePhase::Coast:return "COAST";case AutoTunePhase::Excite2:return "EXCITE2";
    case AutoTunePhase::Approach:return "APPROACH";case AutoTunePhase::NearSp:return "NEAR_SP";
    case AutoTunePhase::Settle:return "SETTLE";case AutoTunePhase::Generate:return "GENERATE";
  }
  return "UNKNOWN";
}
inline const char *autoTuneReasonName(AutoTuneReason reason) {
  switch(reason) {
    case AutoTuneReason::None:return "NONE";case AutoTuneReason::SafetyAbort:return "SAFETY_ABORT";
    case AutoTuneReason::SensorAbort:return "SENSOR_ABORT";case AutoTuneReason::ModeAbort:return "MODE_ABORT";
    case AutoTuneReason::PreheatTimeout:return "PREHEAT_TIMEOUT";case AutoTuneReason::PhaseTimeout:return "PHASE_TIMEOUT";
    case AutoTuneReason::TotalTimeout:return "TOTAL_TIMEOUT";case AutoTuneReason::NonRepeatable:return "NON_REPEATABLE";
    case AutoTuneReason::AmplitudeTooSmall:return "AMPLITUDE_TOO_SMALL";case AutoTuneReason::PeriodTooSmall:return "PERIOD_TOO_SMALL";
    case AutoTuneReason::InvalidKu:return "INVALID_KU";case AutoTuneReason::InvalidGains:return "INVALID_GAINS";
    case AutoTuneReason::SaveFailed:return "SAVE_FAILED";case AutoTuneReason::Success:return "SUCCESS";
    case AutoTuneReason::NoHeadroom:return "NO_HEADROOM";case AutoTuneReason::BaselineUnstable:return "BASELINE_UNSTABLE";
    case AutoTuneReason::NoResponse:return "NO_RESPONSE";case AutoTuneReason::ModelInvalid:return "MODEL_INVALID";
    case AutoTuneReason::PowerLimited:return "POWER_LIMITED";case AutoTuneReason::ApproachTimeout:return "APPROACH_TIMEOUT";
    case AutoTuneReason::RelayFailed:return "RELAY_FAILED";case AutoTuneReason::CandidateInvalid:return "CANDIDATE_INVALID";
    case AutoTuneReason::ValidationFailed:return "VALIDATION_FAILED";
  }
  return "UNKNOWN";
}
// Members the controller calls on whichever engine is compiled in. The legacy relay-only engine gets
// harmless defaults so machine_control.h keeps a single code path.
struct AutoTuneModelView { float gain = 0, delaySec = 0, coast100C = 0, holdPct = 0, ku = 0, periodSec = 0; uint8_t confidence = 0; };
struct AutoTuneEvalView { float overshoot = 0, mae = 0, p95 = 0, ripple = 0; };
struct AutoTuneEngineDefaults {
  AutoTuneModelView model() const { return AutoTuneModelView(); }
  AutoTuneEvalView eval() const { return AutoTuneEvalView(); }
  float candidateKp() const { return 0; }
  float candidateKi() const { return 0; }
  MayapThermal::ThermalProfile profile() const { return MayapThermal::defaultProfile(); }
  MayapThermal::StartupHint startupHint(float, float) const { return MayapThermal::StartupHint(); }
  MayapThermal::Assist assist() const { return MayapThermal::Assist(); }
  void tick(uint32_t, bool) {}
  void noteController(float, float) {}
  void noteSensor(float, bool, bool) {}
  bool externalControl() const { return false; }
  bool preflight(float, const MachineConfig &, const char *&) { return true; }
  bool takeValidationStart(float &) { return false; }
  void applyCandidate(MachineConfig &) const {}
  uint32_t modelSerial() const { return 0; }
  bool hasProfile() const { return false; }
};
class RelayAutoTune : public AutoTuneEngineDefaults {
 public:
  struct Cycle { float high=NAN,low=NAN,amplitude=0; uint32_t periodMs=0,heatMs=0,coolMs=0; };
  struct Result { float amplitude=0,periodSec=0,heatSec=0,coolSec=0,ku=0,gainScale=1; };
  explicit RelayAutoTune(uint8_t preheatPercent=AUTOTUNE_PREHEAT_POWER_PERCENT)
      : preheatPercent_(preheatPercent) {}
  // Smart AutoTune V1 reuses the relay near the setpoint with a power derived from the identified plant.
  void setPreheatPercent(uint8_t percent) { preheatPercent_ = percent; }
  // Smart AutoTune runs the relay on plants far slower than the legacy limits assume (cold start is NOT part of it any more).
  void setLimits(uint32_t totalMs, uint32_t preheatMs, uint32_t phaseMs) { totalMaxMs_ = totalMs; preheatMaxMs_ = preheatMs; phaseMaxMs_ = phaseMs; }
  void configure(float target) { target_=target; }
  void start(uint32_t now,float input) {
    state_=AutoTuneState::Running;phase_=AutoTunePhase::Preheat;reason_=AutoTuneReason::None;
    rejection_=AutoTuneReason::None;startedAt_=phaseStartedAt_=now;preheatMs_=firstUpperMs_=0;
    hasUpper_=false;cycleCount_=0;cycleSerial_=validationSerial_=0;warmupDiscarded_=false;
    currentLow_=currentHigh_=input;capturedHigh_=NAN;lastCycle_=Cycle{};result_=Result{};
    power_=relayHigh_=relayLow_=0;levelsLocked_=false;progress_=1;
    if(!isfinite(input) || !isfinite(target_))abort(AutoTuneReason::SensorAbort);
  }
  void abort(AutoTuneReason reason=AutoTuneReason::SafetyAbort) {
    state_=AutoTuneState::Failed;phase_=AutoTunePhase::Failed;reason_=reason;power_=0;progress_=0;
  }
  void cancel() {
    // Operator cancellation is not a tuning failure. Remove heater demand
    // immediately and return the public state to Idle; start() rebuilds all
    // measurement/cycle state before a later run.
    state_=AutoTuneState::Idle;phase_=AutoTunePhase::Idle;
    reason_=AutoTuneReason::None;rejection_=AutoTuneReason::None;
    power_=relayHigh_=relayLow_=0;progress_=0;levelsLocked_=false;
  }
  // Called every control cycle; timeout enforcement cannot wait for a sensor sample.
  void checkTimeout(uint32_t now) {
    if(!running())return;
    if(elapsedMs(now,startedAt_)>=totalMaxMs_)abort(AutoTuneReason::TotalTimeout);
    else if(phase_==AutoTunePhase::Preheat && elapsedMs(now,phaseStartedAt_)>=preheatMaxMs_)
      abort(AutoTuneReason::PreheatTimeout);
    else if(phase_!=AutoTunePhase::Preheat && elapsedMs(now,phaseStartedAt_)>=phaseMaxMs_)
      abort(AutoTuneReason::PhaseTimeout);
  }
  bool update(uint32_t now,float input,const MachineConfig &cfg,MachineConfig &tunedOut) {
    if(!running())return false;
    if(!isfinite(input)){abort(AutoTuneReason::SensorAbort);return false;}
    checkTimeout(now);if(!running())return false;
    if(!levelsLocked_) {
      relayHigh_=static_cast<float>(std::min<uint8_t>(cfg.autotuneRelayPowerPercent,cfg.maxHeaterPower));
      relayLow_=0;levelsLocked_=true;
    }
    // Changing relay configuration during a measurement invalidates its swing.
    if(relayHigh_!=std::min<uint8_t>(cfg.autotuneRelayPowerPercent,cfg.maxHeaterPower)) {
      abort(AutoTuneReason::ModeAbort);return false;
    }
    if(!isfinite(cfg.autotuneBandC) || cfg.autotuneBandC<=0 || relayHigh_<=relayLow_) {
      abort(AutoTuneReason::InvalidKu);return false;
    }
    if(phase_==AutoTunePhase::Preheat) {
      power_=static_cast<float>(std::min<uint8_t>(preheatPercent_,cfg.maxHeaterPower));
      if(input<target_-cfg.autotuneBandC)return false;
      preheatMs_=elapsedMs(now,startedAt_);
      // No preheat peaks/periods enter measurement. Start fresh near lower band.
      phase_=input>=target_+cfg.autotuneBandC?AutoTunePhase::Cooling:AutoTunePhase::Heating;
      phaseStartedAt_=now;currentLow_=currentHigh_=input;capturedHigh_=NAN;
      power_=phase_==AutoTunePhase::Heating?relayHigh_:relayLow_;progress_=5;
      return false;
    }
    if(phase_==AutoTunePhase::Heating) {
      currentLow_=std::min(currentLow_,input);power_=relayHigh_;
      if(input<target_+cfg.autotuneBandC)return false;
      if(hasUpper_ && isfinite(capturedHigh_)) {
        lastCycle_.high=capturedHigh_;lastCycle_.low=currentLow_;
        lastCycle_.amplitude=(capturedHigh_-currentLow_)*0.5f;
        lastCycle_.periodMs=elapsedMs(now,lastUpperCrossAt_);
        lastCycle_.heatMs=elapsedMs(now,phaseStartedAt_);
        lastCycle_.coolMs=capturedCoolMs_;++cycleSerial_;
        if(lastCycle_.amplitude<AUTOTUNE_MIN_AMPLITUDE_C)rejection_=AutoTuneReason::AmplitudeTooSmall;
        else if(lastCycle_.periodMs<AUTOTUNE_MIN_PERIOD_MS)rejection_=AutoTuneReason::PeriodTooSmall;
        else if(!warmupDiscarded_)warmupDiscarded_=true;
        else {cycles_[cycleCount_++]=lastCycle_;progress_=static_cast<uint8_t>(cycleCount_*90U/AUTOTUNE_REQUIRED_CYCLES);}
      }
      if(!hasUpper_)firstUpperMs_=elapsedMs(now,startedAt_);
      hasUpper_=true;lastUpperCrossAt_=now;
      phase_=AutoTunePhase::Cooling;phaseStartedAt_=now;currentHigh_=input;power_=relayLow_;
    } else if(phase_==AutoTunePhase::Cooling) {
      currentHigh_=std::max(currentHigh_,input);power_=relayLow_;
      if(input>target_-cfg.autotuneBandC)return false;
      capturedHigh_=currentHigh_;capturedCoolMs_=elapsedMs(now,phaseStartedAt_);
      phase_=AutoTunePhase::Heating;phaseStartedAt_=now;currentLow_=input;power_=relayHigh_;
    }
    if(cycleCount_<AUTOTUNE_REQUIRED_CYCLES)return false;
    phase_=AutoTunePhase::Validating;++validationSerial_;result_=Result{};
    for(uint8_t i=0;i<AUTOTUNE_REQUIRED_CYCLES;++i) {
      result_.amplitude+=cycles_[i].amplitude/AUTOTUNE_REQUIRED_CYCLES;
      result_.periodSec+=cycles_[i].periodMs*0.001f/AUTOTUNE_REQUIRED_CYCLES;
      result_.heatSec+=cycles_[i].heatMs*0.001f/AUTOTUNE_REQUIRED_CYCLES;
      result_.coolSec+=cycles_[i].coolMs*0.001f/AUTOTUNE_REQUIRED_CYCLES;
    }
    for(uint8_t i=0;i<AUTOTUNE_REQUIRED_CYCLES;++i) {
      if(fabsf(cycles_[i].amplitude-result_.amplitude)>result_.amplitude*AUTOTUNE_STABILITY_FRACTION ||
         fabsf(cycles_[i].periodMs*0.001f-result_.periodSec)>result_.periodSec*AUTOTUNE_STABILITY_FRACTION) {
        // Preserve rolling-window qualification; expose the rejected quality.
        rejection_=AutoTuneReason::NonRepeatable;
        for(uint8_t n=1;n<AUTOTUNE_REQUIRED_CYCLES;++n)cycles_[n-1]=cycles_[n];
        cycleCount_=AUTOTUNE_REQUIRED_CYCLES-1;phase_=AutoTunePhase::Cooling;return false;
      }
    }
    const float d=(relayHigh_-relayLow_)*0.5f; // Actual locked commanded swing, NEVER preheat.
    result_.ku=4*d/(static_cast<float>(PI)*result_.amplitude);
    if(!isfinite(result_.ku) || result_.ku<=0 || !isfinite(result_.periodSec) || result_.periodSec<=0) {
      abort(AutoTuneReason::InvalidKu);return false;
    }
    // Preserve Tyreus-Luyben coefficients and proportional gain scaling.
    const float kp=result_.ku/2.2f,ki=kp/(2.2f*result_.periodSec),kd=kp*result_.periodSec/6.3f;
    result_.gainScale=fmaxf(1.0f,fmaxf(kp/100.0f,fmaxf(ki/20.0f,kd/200.0f)));
    // Round the common scale UP by one float ULP, so an exact boundary (e.g.
    // Kd=200) cannot divide to 200.000015 and be rejected/clipped by sanitize.
    if(result_.gainScale>1.0f && isfinite(result_.gainScale))
      result_.gainScale=nextafterf(result_.gainScale, INFINITY);
    if(!isfinite(kp) || !isfinite(ki) || !isfinite(kd) || !isfinite(result_.gainScale) ||
       kp/result_.gainScale<0.1f || ki<=0 || kd<=0) {abort(AutoTuneReason::InvalidGains);return false;}
    MachineConfig candidate=cfg;candidate.controlMode=ControlMode::Pid;
    candidate.kp=kp/result_.gainScale;candidate.ki=ki/result_.gainScale;candidate.kd=kd/result_.gainScale;
    if(candidate.kp>100 || candidate.ki>20 || candidate.kd>200) {abort(AutoTuneReason::InvalidGains);return false;}
    const float p=candidate.kp,i=candidate.ki,dGain=candidate.kd;
    sanitizeMachineConfig(candidate);
    if(candidate.kp!=p || candidate.ki!=i || candidate.kd!=dGain) {abort(AutoTuneReason::InvalidGains);return false;}
    tunedOut=candidate;state_=AutoTuneState::Success;phase_=AutoTunePhase::Success;
    reason_=AutoTuneReason::Success;power_=0;progress_=100;return true;
  }
  AutoTuneState state()const{return state_;}
  AutoTunePhase phase()const{return phase_;}
  AutoTuneReason reason()const{return reason_;}
  AutoTuneReason rejection()const{return rejection_;}
  uint8_t progress()const{return progress_;}
  float power()const{return power_;}
  float relayHigh()const{return relayHigh_;}
  float relayLow()const{return relayLow_;}
  uint8_t cycleCount()const{return cycleCount_;}
  uint32_t cycleSerial()const{return cycleSerial_;}
  uint32_t validationSerial()const{return validationSerial_;}
  uint32_t preheatMs()const{return preheatMs_;}
  uint32_t firstUpperMs()const{return firstUpperMs_;}
  const Cycle &lastCycle()const{return lastCycle_;}
  const Result &result()const{return result_;}
  bool running()const{return state_==AutoTuneState::Running;}
 private:
  uint8_t preheatPercent_;
  uint32_t totalMaxMs_=AUTOTUNE_TOTAL_MAX_MS,preheatMaxMs_=AUTOTUNE_PREHEAT_MAX_MS,phaseMaxMs_=AUTOTUNE_PHASE_MAX_MS;
  AutoTuneState state_=AutoTuneState::Idle;
  AutoTunePhase phase_=AutoTunePhase::Idle;
  AutoTuneReason reason_=AutoTuneReason::None,rejection_=AutoTuneReason::None;
  float target_=37.5f,power_=0,relayHigh_=0,relayLow_=0;
  uint32_t startedAt_=0,phaseStartedAt_=0,lastUpperCrossAt_=0,capturedCoolMs_=0;
  uint32_t preheatMs_=0,firstUpperMs_=0,cycleSerial_=0,validationSerial_=0;
  bool hasUpper_=false,levelsLocked_=false,warmupDiscarded_=false;
  float currentLow_=NAN,currentHigh_=NAN,capturedHigh_=NAN;
  Cycle cycles_[AUTOTUNE_REQUIRED_CYCLES]{};Cycle lastCycle_{};Result result_{};
  uint8_t cycleCount_=0,progress_=0;
};
