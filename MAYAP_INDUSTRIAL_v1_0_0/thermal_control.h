#pragma once
#include "thermal_assist.h"

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
    integral_ = 0.0f;
    lastInput_ = 0.0f;
    lastComputeAt_ = 0;
    output_ = 0.0f;
    filteredDerivative_ = 0.0f;
    ffApplied_ = 0.0f;
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
      integral_ = clampFloat(output_ - ffApplied_ - cfg.kp * (beta_ * setpoint - input),
                            -integralLimit, integralLimit);
    } else {
      integral_ = 0.0f;
    }
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

    const float p = cfg.kp * (beta_ * setpoint - input);
    // First-order derivative filter: sensor noise must not command full SSR
    // swings. Derivative stays on PV, avoiding setpoint derivative kick.
    filteredDerivative_ += (dt / (PID_D_FILTER_TAU_SEC + dt)) *
                           (dInput - filteredDerivative_);
    const float d = -cfg.kd * filteredDerivative_;
    // Weighted absolute Celsius P has a DC offset. Permit I to cancel it:
    // the old +/-maxOut bound alone can prevent beta<1 reaching the setpoint.
    // Anti-windup uses the ACTUAL 0..maxOut actuator limits.
    const float integralLimit = maxOut + fabsf(cfg.kp * (1.0f - beta_) * setpoint);
    // The integral carries the RESIDUAL: when the feed-forward moves, the integral moves
    // the opposite way so the TOTAL is continuous (bumpless, no double counting of hold).
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
    output_ = clampFloat(pp + integral_ + d, lo, hi);
    return output_;
  }

  float output() const { return output_; }
  float integral() const { return integral_; }
  float feedForwardApplied() const { return ffApplied_; }

 private:
  const float beta_;
  bool initialized_ = false;
  float integral_ = 0.0f;
  float filteredDerivative_ = 0.0f;
  float lastInput_ = 0.0f;
  uint32_t lastComputeAt_ = 0;
  float output_ = 0.0f;
  float ffApplied_ = 0.0f;
};

// Physical heat is metered from the arbiter's actual SSR state, not PID demand.
// The bounded 2 s buckets retain 240 s of energy and no credit across a cut.
class ThermalStartupController {
 public:
  enum class Phase : uint8_t { FullHeat, Approach, SoftLanding, Hold };
  void reset() {
    for (uint8_t i = 0; i < Buckets; ++i) onMs_[i] = 0;
    phase_ = Phase::FullHeat; initialized_ = false; observed_ = false; historyGap_ = false;
    bucket_ = 0; firstHeatAt_ = 0; firstRiseAt_ = 0;
    lastHeatOffAt_ = 0; energyWindowStartedAt_ = 0; coastCleared_ = false;
    stableAt_ = 0; lastRequested_ = 0; holdPower_ = 0;
    slope_ = 0; lastSampleAt_ = 0; lastPeak_ = 0;
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
      if (lastOn_) onMs_[bucket_] += part;
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
  struct Decision { float ceiling; bool freezePositiveIntegral; Phase phase; float peak; };
  // Learned plant knowledge from Adaptive Thermal V1. An invalid hint is exactly the
  // legacy controller: it replaces the lightest-plant coast assumption, never adds heat.
  void setHint(const MayapThermal::StartupHint &h) { hint_ = h; }
  Decision decide(uint32_t now, float sp, float pv, float maxPower) {
    if (!isfinite(sp) || !isfinite(pv)) return {0, true, phase_, pv};
    historyGap_ = false; // only a fresh real sensor sample can resume heat
    if (!initialized_) {
      initialized_ = true; lastPv_ = pv; lastSp_ = sp;
      lastSampleAt_ = now;
    }
    const float dt = clampFloat(static_cast<float>(static_cast<uint32_t>(now-lastSampleAt_))*0.001f, 0.25f, 10.0f);
    const float measuredRate = (pv-lastPv_)/dt;
    slope_ += dt/(10.0f+dt)*(measuredRate-slope_);
    lastPv_ = pv; lastSampleAt_ = now;
    if (sp != lastSp_) { stableAt_ = 0U; phase_ = Phase::Approach; lastSp_ = sp; }
    if (firstRiseAt_ == 0U && firstHeatAt_ != 0U && slope_ > 0.003f &&
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
    if (!coastCleared_ && !lastOn_ && lastHeatOffAt_ != 0U &&
        static_cast<uint32_t>(now-lastHeatOffAt_) >= static_cast<uint32_t>(coastSeconds*1000.0f) &&
        slope_ <= 0.001f) {
      for (uint8_t i = 0; i < Buckets; ++i) onMs_[i] = 0;
      energyWindowStartedAt_ = now;
      coastCleared_ = true;
    }
    // Quantized sensors (0.1 C steps) make the 10 s IIR slope spike on every step, which
    // fakes coast. With a learned hint the window-regression slope replaces it.
    const float slopeEff = (hs > 0.0f && hint_.slopeValid) ? slope_ + hs*(hint_.slopePerSec - slope_) : slope_;
    const float slopeCoast = fmaxf(0.0f, slopeEff) * coastSeconds;
    // The long-term loss-compensation duty is deliberately slow and cannot
    // be learned until after the first delayed heat response. It removes the
    // permanent offset of proportional-only braking without banking pulse
    // energy for a later burst.
    if (firstRiseAt_ != 0U && (!longDelay || coastCleared_ || sp-pv > 1.0f) &&
        static_cast<uint32_t>(now-firstRiseAt_) >= (longDelay ? 240000U : 180000U)) {
      if (sp-pv > 0.08f && slope_ <= 0.003f)
        holdPower_ += dt/60.0f;
      else if (sp-pv < -0.05f || slope_ > 0.006f)
        holdPower_ -= 3.0f*dt/60.0f;
      holdPower_ = clampFloat(holdPower_,0.0f,maxPower);
    }
    uint32_t horizonMs = longDelay ? 240000U : 160000U;
    if (hs >= 0.5f)  // only energy still IN FLIGHT can coast: window = learned delay + margin
      horizonMs = static_cast<uint32_t>(clampFloat((hint_.delaySec+6.0f)*0.5f, 6.0f, 120.0f))*2000U;
    // Learned hold power (average ACTUAL duty at the setpoint) replaces the slow legacy
    // integrator in proportion to the hint strength.
    const float holdBase = hs > 0.0f ? holdPower_ + hs*(clampFloat(hint_.holdPct,0.0f,maxPower)-holdPower_) : holdPower_;
    // A known vent removes heat: the duty needed just to stand still is higher while it runs.
    const float holdEff = clampFloat(holdBase + (hs > 0.0f ? hs*hint_.ventPct : 0.0f), 0.0f, maxPower);
    const uint8_t recentBuckets = static_cast<uint8_t>(horizonMs/2000U);
    uint32_t recentOnMs = 0;
    for (uint8_t i = 0; i < recentBuckets; ++i)
      recentOnMs += onMs_[(bucket_+Buckets-i)%Buckets];
    const float windowMs = static_cast<float>(std::min<uint32_t>(horizonMs,
        static_cast<uint32_t>(now-energyWindowStartedAt_)));
    const float excessOnMs = fmaxf(0.0f, static_cast<float>(recentOnMs) -
        holdEff * 0.01f * windowMs);
    // Rated watts are not a calibration of transfer to this one probe.
    // Keep a bounded 35% reserve for heater effectiveness, sensor filter lag
    // and delayed heat before trusting a first-rise estimate.
    // Do not subtract PV rise over this ring: with transport delay, that rise
    // can be caused by an older pulse which already aged out of the ring.
    const float legacyEnergyCoast = excessOnMs*16.0f*1.35f/MinimumCapacity;
    const float energyCoast = hs > 0.0f && hint_.coastPerOnMs > 0.0f
        ? legacyEnergyCoast + hs*(excessOnMs*hint_.coastPerOnMs - legacyEnergyCoast)
        : legacyEnergyCoast;
    const float expectedCoast = fmaxf(slopeCoast, energyCoast);
    const float error = sp-pv;
    const float peak = pv+expectedCoast;
    lastPeak_ = peak;
    if (fabsf(error) <= 0.45f && fabsf(slope_) <= 0.002f) {
      if (stableAt_ == 0U) stableAt_ = now;
      if (static_cast<uint32_t>(now-stableAt_) >= 60000U) phase_ = Phase::Hold;
    } else stableAt_ = 0U;
    if (phase_ == Phase::Hold && (error > 0.7f || error < -0.25f || slope_ > 0.006f))
      phase_ = Phase::Approach;
    if (phase_ != Phase::Hold) {
      if (error > 2.0f && peak < sp-1.0f) phase_ = Phase::FullHeat;
      else if (error > 0.8f && peak < sp-0.3f) phase_ = Phase::Approach;
      else phase_ = Phase::SoftLanding;
    }
    // HOLD is still a delayed 16 kW plant. Never bypass the same braking
    // envelope merely because the filtered PV was momentarily stationary.
    const float remaining = error-expectedCoast;
    const float brakeFraction=clampFloat((remaining+0.05f)/0.5f, 0.0f, 1.0f);
    // The predictive fraction refers to the rated bank. External authority
    // remains a separate final limit and never redefines physical 100%.
    float cap = 100.0f*brakeFraction;
    if (hs > 0.0f) {
      // Hold-aware braking: the legacy law is a pure proportional cap that is unaware of the
      // duty needed just to stand still, so a plant with 33 % hold power can only balance at
      // error >= ~0.07 C (more for bigger hold). Brake the SURPLUS above the learned hold
      // instead; go below hold only when the predicted coast really overshoots SP.
      const float f = clampFloat((remaining+0.05f)/0.5f, -1.0f, 1.0f);
      const float holdCap = clampFloat(holdEff, 0.0f, maxPower);
      const float learnedCap = f >= 0.0f ? holdCap + (maxPower-holdCap)*f : holdCap*(1.0f+f);
      cap += hs*(learnedCap - cap);
    }
    // Near SP, a stationary heavy load can suddenly become light before the
    // remote sensor sees it. Allow only a bounded increment above established
    // maintenance duty until the changed slope is observed.
    if (error < 2.0f) {
      const float legacyFloor = fmaxf(20.0f, holdEff+5.0f);
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
    if (slopeEff > 0.0f) {
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
    lastCap_ = cap;
    const bool freezeIntegral = phase_ != Phase::Hold &&
        !(error > 0.15f && slope_ <= 0.001f && peak < sp-0.2f);
    return {cap, freezeIntegral, phase_, peak};
  }
  void requested(float power) { lastRequested_ = fmaxf(0.0f, power); }
  bool sampleStale(uint32_t now) const {
    return historyGap_ ||
        (initialized_ && static_cast<uint32_t>(now-lastSampleAt_) > 6000U);
  }
  Phase phase() const { return phase_; }
  float predictedPeak() const { return lastPeak_; }
  float lastCeiling() const { return lastCap_; }
  float slope() const { return slope_; }
 private:
  static constexpr uint8_t Buckets = 120U;
  static constexpr float MinimumCapacity = 180000.0f;
  uint32_t onMs_[Buckets]{};
  uint32_t observedAt_ = 0, bucketAt_ = 0;
  uint32_t firstHeatAt_ = 0, firstRiseAt_ = 0, lastHeatOffAt_ = 0;
  uint32_t energyWindowStartedAt_ = 0, stableAt_ = 0, lastSampleAt_ = 0;
  uint8_t bucket_ = 0;
  bool initialized_ = false, observed_ = false, lastOn_ = false, coastCleared_ = false;
  bool historyGap_ = false;
  float lastPv_ = 0, lastSp_ = 0, slope_ = 0;
  float lastRequested_ = 0, holdPower_ = 0, lastPeak_ = 0, lastCap_ = 0;
  MayapThermal::StartupHint hint_{};
  Phase phase_ = Phase::FullHeat;
};

// These phases/reasons are service diagnostics, not changes to public state codes.
enum class AutoTunePhase : uint8_t { Idle, Preheat, Heating, Cooling, Validating, Success, Failed };
enum class AutoTuneReason : uint8_t {
  None, SafetyAbort, SensorAbort, ModeAbort, PreheatTimeout, PhaseTimeout,
  TotalTimeout, NonRepeatable, AmplitudeTooSmall, PeriodTooSmall,
  InvalidKu, InvalidGains, SaveFailed, Success
};
inline const char *autoTunePhaseName(AutoTunePhase phase) {
  switch(phase) {
    case AutoTunePhase::Idle:return "IDLE";case AutoTunePhase::Preheat:return "PREHEAT";
    case AutoTunePhase::Heating:return "HEATING";case AutoTunePhase::Cooling:return "COOLING";
    case AutoTunePhase::Validating:return "VALIDATING";case AutoTunePhase::Success:return "SUCCESS";
    case AutoTunePhase::Failed:return "FAILED";
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
  }
  return "UNKNOWN";
}
class RelayAutoTune {
 public:
  struct Cycle { float high=NAN,low=NAN,amplitude=0; uint32_t periodMs=0,heatMs=0,coolMs=0; };
  struct Result { float amplitude=0,periodSec=0,heatSec=0,coolSec=0,ku=0,gainScale=1; };
  explicit RelayAutoTune(uint8_t preheatPercent=AUTOTUNE_PREHEAT_POWER_PERCENT)
      : preheatPercent_(preheatPercent) {}
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
    if(elapsedMs(now,startedAt_)>=AUTOTUNE_TOTAL_MAX_MS)abort(AutoTuneReason::TotalTimeout);
    else if(phase_==AutoTunePhase::Preheat && elapsedMs(now,phaseStartedAt_)>=AUTOTUNE_PREHEAT_MAX_MS)
      abort(AutoTuneReason::PreheatTimeout);
    else if(phase_!=AutoTunePhase::Preheat && elapsedMs(now,phaseStartedAt_)>=AUTOTUNE_PHASE_MAX_MS)
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
  const uint8_t preheatPercent_;
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
