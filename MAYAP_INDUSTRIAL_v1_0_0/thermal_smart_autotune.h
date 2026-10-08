#pragma once
// Smart AutoTune V1: ACTIVE IDENTIFICATION of the oven that is installed, then a model-based PID
// candidate that must pass a closed-loop validation before anything is saved.
//
//   BASELINE -> EXCITE (low power) -> COAST -> [EXCITE2] -> APPROACH -> NEAR_SP (relay, reused)
//            -> SETTLE -> GENERATE -> VALIDATING -> SUCCESS | FAILED
//
// Identification never trusts "requested %": the plant gain is the PV slope per ACTUAL heater ON-second
// (tick() is fed the arbiter's real SSR state). Nothing in this file knows heater kW, chamber volume or
// the simulator. "Not sure -> do not accept": every doubt ends in FAILED and the caller keeps the old PID.
//
// Pure C++: the host simulation links this same header. It relies on the including unit for
// MachineConfig, AutoTuneState/Phase/Reason, RelayAutoTune, elapsedMs and sanitizeMachineConfig.
#include "thermal_profile.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace SmartTune {

// ---- timing (all bounded; no state waits forever) -------------------------------------------------------
constexpr uint32_t BaselineMs = 90000UL;
constexpr uint32_t ExciteMinMs = 60000UL;
constexpr uint32_t ExciteMaxMs = 720000UL;
constexpr uint32_t CoastMaxMs = 480000UL;
constexpr uint32_t CoastPlateauMs = 60000UL;       // no new PV maximum for this long = the peak is behind us
constexpr uint32_t ApproachMaxMs = 5400000UL;
constexpr uint32_t NearSpMaxMs = 5000000UL;        // the reused relay also keeps its own 45 min / 15 min limits
constexpr uint32_t SettleMaxMs = 900000UL;
constexpr uint32_t SettleCoolMs = 600000UL;      // wait this long at most for PV to fall SettleBelowC under the setpoint
constexpr float SettleBelowC = 0.8f;
constexpr uint32_t ValidateMs = 2700000UL;         // 45 min closed-loop check (at least; settle budget 30 min + tail)
constexpr uint32_t ValidateMaxTailMs = 3000000UL;
constexpr uint32_t ValidateTailMs = 900000UL;      // the last 15 min are scored (at least) ...
constexpr float ValidateTailPeriods = 2.5f;        // ... or 2.5 relay periods when the loop is slower than that
constexpr uint32_t TotalMaxMs = 16200000UL;        // 270 min hard cap (a cold, heavy, weak oven needs > 80 min just to heat)

// ---- safety / quality limits ------------------------------------------------------------------------------
constexpr float MinHeadroomC = 2.5f;       // SP - PV needed to start: the first excitation needs room to coast
constexpr float BaselineMaxSlope = 0.0015f;// degC/s
constexpr float BaselineMaxStd = 0.12f;    // degC, sample-to-sample
constexpr float BaselineMaxJump = 0.6f;    // degC between two samples
constexpr float ExcitePct = 25.0f;
constexpr float Excite2Pct = 35.0f;        // hard cap for any second excitation
constexpr float RiseCapC = 2.0f;           // stop an excitation after this much rise ...
constexpr float RiseCapHeadroom = 0.35f;   // ... or this fraction of the headroom
constexpr float GoodRelSe = 0.10f;         // gain standard error / gain
constexpr float GoodSeparation = 1.15f;
constexpr float RampCoversDelays = 4.0f;   // excitation lasts at least this many apparent delays    // SSE(second-best delay) / SSE(best delay)
constexpr float MinRiseC = 0.5f;
constexpr float CoastRatioMin = 0.4f, CoastRatioMax = 1.7f;   // measured coast / (gain * duty * delay)
constexpr float RelayCoastMarginC = 0.25f; // room kept under the High alarm beyond the relay's own overshoot
constexpr float ApproachBrakeC = 0.6f;
constexpr float ApproachGain = 40.0f;        // % per degC per (1 / coast100): brake loop gain 0.4
constexpr float ValidOvershootC = 0.30f;
constexpr float ValidMae = 0.12f, ValidP95 = 0.18f, ValidRipple = 0.30f;
constexpr float ValidSatFrac = 0.30f;
constexpr float HoldMaxPct = 30.0f;        // a plant that needs more than 30 % duty just to stand still has < 3.3x authority margin: not accepted in V1
constexpr float ApproachArriveC = 0.15f;   // PV moved less than this over the last 200 s = equilibrium
constexpr float KpMax = 60.0f, KpMin = 1.0f, KiMax = 3.0f, KiMin = 0.002f;
constexpr float TauCFactor = 1.5f;       // closed-loop time constant = 1.5 x apparent delay (SIMC "smooth"; 1.0 = aggressive)
constexpr float ConfBase = 60.0f, ConfSpan = 10.0f;   // measured-profile confidence 60..70: above the aged-seed cap (40) and the 60 qualification line, well under the 100 a long-run profile can earn
constexpr float KuMargin = 2.2f;           // Kp <= Ku / 2.2 (gain margin >= 2.2)

constexpr uint8_t GridN = 13;
constexpr float Grid[GridN] = {4, 8, 14, 20, 28, 38, 50, 65, 82, 102, 126, 155, 190};

// Fixed-memory identification of  dPV/dt = Kh * u(t - theta)  during a CONSTANT-power excitation: PV rise against the ideal
// delayed ON-seconds  x_j(t) = d * max(0, t - theta_j)  (d = commanded duty), one running (Welford) regression per delay candidate.
// No sample history at all: the actual/commanded duty ratio over the whole excitation rescales the gain afterwards (see coast()).
class StepEstimator {
 public:
  void reset(float pv0, uint32_t startMs, float duty) {
    pv0_ = pv0; start_ = startMs; duty_ = duty; rise_ = 0; used_ = 0;
    for (uint8_t i = 0; i < GridN; ++i) { n_[i] = 0; mx_[i] = my_[i] = cxx_[i] = cxy_[i] = cyy_[i] = 0; }
  }
  void sample(uint32_t now, float pv) {
    const float t = static_cast<float>(now - start_) * 0.001f;
    const float y = pv - pv0_;
    if (y > rise_) rise_ = y;
    for (uint8_t i = 0; i < GridN; ++i) {
      const float x = duty_ * std::max(0.0f, t - Grid[i]);
      n_[i] += 1.0f;
      const float dx = x - mx_[i];
      mx_[i] += dx / n_[i];
      const float dy = y - my_[i];
      my_[i] += dy / n_[i];
      cxx_[i] += dx * (x - mx_[i]);
      cxy_[i] += dx * (y - my_[i]);
      cyy_[i] += dy * (y - my_[i]);
    }
    ++used_;
  }
  struct Fit {
    bool valid = false;
    float gain = 0, delay = 0, relSe = 1, separation = 1, rise = 0;
    uint16_t n = 0;
  };
  Fit fit() const {
    Fit f;
    f.rise = rise_; f.n = static_cast<uint16_t>(used_);
    float sse[GridN];
    uint8_t best = 0;
    bool any = false;
    for (uint8_t i = 0; i < GridN; ++i) {
      if (cxx_[i] > 1e-3f && cxy_[i] > 0.0f) sse[i] = std::max(0.0f, cyy_[i] - cxy_[i] * cxy_[i] / cxx_[i]);
      else sse[i] = cyy_[i] > 0.0f ? cyy_[i] : 1e-6f;
      if (cxx_[i] > 1e-3f && cxy_[i] > 0.0f && (!any || sse[i] < sse[best])) { best = i; any = true; }
    }
    if (!any || used_ < 12U) return f;
    float second = 1e30f;
    for (uint8_t i = 0; i < GridN; ++i)
      if ((i + 1U < best || i > best + 1U) && sse[i] < second) second = sse[i];
    f.separation = sse[best] > 1e-9f ? second / sse[best] : 10.0f;
    f.gain = cxy_[best] / cxx_[best];
    // parabolic refinement of the delay between the neighbouring candidates
    float theta = Grid[best];
    if (best > 0 && best + 1U < GridN) {
      const float x0 = Grid[best - 1], x1 = Grid[best], x2 = Grid[best + 1];
      const float y0 = sse[best - 1], y1 = sse[best], y2 = sse[best + 1];
      const float den = (x0 - x1) * (x0 - x2) * (x1 - x2);
      if (std::fabs(den) > 1e-6f) {
        const float a = (x2 * (y1 - y0) + x1 * (y0 - y2) + x0 * (y2 - y1)) / den;
        const float b = (x2 * x2 * (y0 - y1) + x1 * x1 * (y2 - y0) + x0 * x0 * (y1 - y2)) / den;
        if (a > 1e-9f) {
          const float vertex = -b / (2.0f * a);
          if (std::isfinite(vertex)) theta = std::min(x2, std::max(x0, vertex));
        }
      }
    }
    f.delay = theta;
    const float dof = std::max(1.0f, n_[best] - 2.0f);
    const float se = std::sqrt(std::max(0.0f, sse[best]) / dof / cxx_[best]);
    f.relSe = f.gain > 1e-9f ? se / f.gain : 1.0f;
    f.valid = std::isfinite(f.gain) && std::isfinite(f.delay) && std::isfinite(f.relSe) && f.gain > 0.0f;
    return f;
  }
 private:
  float duty_ = 0.25f;
 private:
  float pv0_ = 0, rise_ = 0;
  uint32_t start_ = 0, used_ = 0;
  float n_[GridN], mx_[GridN], my_[GridN], cxx_[GridN], cxy_[GridN], cyy_[GridN];
};

// Model-based PI from the identified plant (pure; unit-tested).
//   Plant:  PV' = k' * u(t - theta),   k' = Kh / 100  [degC/s per % duty],  theta = apparent delay + 4 s (sampling / burst quantum)
//   SIMC for an integrating process with delay (Skogestad), tauC = TauCFactor * theta (>= 20 s):
//        Kc   = 1 / (k' * (tauC + theta))          [% per degC]
//        tauI = 4 * (tauC + theta)                  [s]
//        Ki   = Kc / tauI                           [% per (degC * s)]      Kd = 0 (no clear benefit on a thermal plant with a 0.01-0.1 C probe)
//   Sanitising: Kp <= Ku_relay / 2.2 (gain margin) when the relay measured Ku, Kp in [1, 60], Ki = Kp / tauI in [0.002, 3]; any NaN/Inf or a
//   relay period shorter than 2 theta -> invalid. Returns false for "do not use".
inline bool simc(float gain, float delaySec, float ku, float periodSec, float &kp, float &ki, float &tauI) {
  if (!std::isfinite(gain) || !std::isfinite(delaySec) || gain <= 0.0f) return false;
  const float theta = std::max(6.0f, delaySec + 4.0f);
  const float tauC = std::max(TauCFactor * theta, 20.0f);
  const float kPrime = gain * 0.01f;
  kp = 1.0f / (kPrime * (tauC + theta));
  tauI = 4.0f * (tauC + theta);
  if (!std::isfinite(kp) || kp <= 0.0f || !std::isfinite(tauI)) return false;
  if (ku > 0.0f) kp = std::min(kp, ku / KuMargin);
  if (periodSec > 0.0f && periodSec < 2.0f * theta) return false;
  kp = std::min(KpMax, kp);
  ki = std::max(KiMin, std::min(KiMax, kp / tauI));
  return kp >= KpMin && std::isfinite(ki);
}

struct Model {
  float gain = 0;          // degC per ON-second == degC/s at 100 % ACTUAL duty (effective plant gain)
  float delaySec = 0;      // apparent delay, SSR on -> reliable sensor response
  float coastRiseC = 0;    // measured coast after the excitation cut
  float coast100C = 0;     // the same scaled to a 100 % -> 0 % cut
  float coastSec = 0;
  float holdPct = 0;       // mean ACTUAL duty around the setpoint (relay), 0 = unknown
  float holdEqPct = 0;     // mean ACTUAL duty at the approach equilibrium (just under the setpoint)
  float relSe = 1, separation = 1, coastRatio = 0;
  float ku = 0, periodSec = 0;
  float tauI = 0;
  uint8_t confidence = 0;
  bool valid = false;
};

}  // namespace SmartTune

class SmartAutoTune {
 public:
  explicit SmartAutoTune(uint8_t /*legacy preheat percent, unused*/ = 0) {}
  using Cycle = RelayAutoTune::Cycle;
  using Result = RelayAutoTune::Result;
  struct Eval {   // closed-loop validation scores
    float overshoot = 0, mae = 0, p95 = 0, ripple = 0, satFrac = 0, integralPeak = 0;
    uint32_t samples = 0;
    bool ran = false, pass = false;
  };

  void configure(float target) { target_ = target; }

  // Start only from a cold-enough chamber: the first excitation needs room before the setpoint.
  bool preflight(float pv, const MachineConfig &cfg, const char *&message) {
    if (!std::isfinite(pv)) { message = "CAM BIEN CHUA SAN SANG"; return false; }
    if (cfg.targetTemp - pv < SmartTune::MinHeadroomC) { message = "LO CAN NGUOI TRUOC KHI TUNE"; return false; }
    return true;
  }

  void start(uint32_t now, float input) {
    state_ = AutoTuneState::Running; phase_ = AutoTunePhase::Baseline;
    reason_ = AutoTuneReason::None; rejection_ = AutoTuneReason::None;
    startedAt_ = phaseAt_ = now;
    power_ = 0; progress_ = 1; used2_ = false; validationSerial_ = modelSerial_ = 0;
    model_ = SmartTune::Model(); eval_ = Eval();
    onMs_ = 0; tickAt_ = now; tickOn_ = false; tickSeen_ = false;
    slopeCount_ = 0; slopeHead_ = 0; nearAt_ = 0;
    baseN_ = 0; baseSum_ = baseSq_ = 0; basePrev_ = input; baseFirst_ = input; baseFirstAt_ = now; baseMaxJump_ = 0;
    pvOff_ = pvPeak_ = input; validationStartPending_ = false;
    relayRunning_ = false; relayHigh_ = 0; candidateValid_ = false; cand_ = Candidate();
    if (!std::isfinite(input) || !std::isfinite(target_)) abort(AutoTuneReason::SensorAbort);
  }
  void abort(AutoTuneReason reason = AutoTuneReason::SafetyAbort) {
    state_ = AutoTuneState::Failed; phase_ = AutoTunePhase::Failed; reason_ = reason;
    power_ = 0; progress_ = 0; validationStartPending_ = false; candidateValid_ = false;
    if (relayRunning_) { relay_.cancel(); relayRunning_ = false; }
  }
  void cancel() {
    if (relayRunning_) { relay_.cancel(); relayRunning_ = false; }
    state_ = AutoTuneState::Idle; phase_ = AutoTunePhase::Idle;
    reason_ = rejection_ = AutoTuneReason::None;
    power_ = 0; progress_ = 0; validationStartPending_ = false; candidateValid_ = false;
  }
  // Every control cycle: timeouts cannot wait for a sensor sample.
  void checkTimeout(uint32_t now) {
    if (!running()) return;
    if (elapsedMs(now, startedAt_) >= SmartTune::TotalMaxMs) { abort(AutoTuneReason::TotalTimeout); return; }
    const uint32_t inPhase = elapsedMs(now, phaseAt_);
    uint32_t limit = 0;
    switch (phase_) {
      case AutoTunePhase::Baseline: limit = SmartTune::BaselineMs + 30000UL; break;
      case AutoTunePhase::Excite: case AutoTunePhase::Excite2: limit = SmartTune::ExciteMaxMs + 30000UL; break;
      case AutoTunePhase::Coast: limit = SmartTune::CoastMaxMs + 30000UL; break;
      case AutoTunePhase::Approach: limit = SmartTune::ApproachMaxMs; break;
      case AutoTunePhase::NearSp: limit = SmartTune::NearSpMaxMs; break;
      case AutoTunePhase::Settle: limit = SmartTune::SettleMaxMs; break;
      case AutoTunePhase::Validating: limit = validateMs_ + 60000UL; break;
      default: return;
    }
    if (inPhase >= limit) { abort(phase_ == AutoTunePhase::Approach ? AutoTuneReason::ApproachTimeout : AutoTuneReason::PhaseTimeout); return; }
    if (phase_ == AutoTunePhase::NearSp && relayRunning_) {
      relay_.checkTimeout(now);
      if (relay_.state() == AutoTuneState::Failed) {
        rejection_ = relay_.reason();
        abort(relay_.reason() == AutoTuneReason::SafetyAbort ? AutoTuneReason::SafetyAbort : AutoTuneReason::RelayFailed);
      }
    }
  }
  // Every control cycle with the arbiter's ACTUAL heater state (SSR on and master picked up).
  void tick(uint32_t now, bool actualOn) {
    if (!running()) return;
    if (tickSeen_ && tickOn_) onMs_ += std::min<uint32_t>(elapsedMs(now, tickAt_), 1000UL);
    tickAt_ = now; tickOn_ = actualOn; tickSeen_ = true;
  }
  void noteController(float requestedPct, float integral) { reqPct_ = requestedPct; integral_ = integral; }

  bool update(uint32_t now, float pv, const MachineConfig &cfg, MachineConfig &tunedOut) {
    if (!running()) return false;
    if (!std::isfinite(pv)) { abort(AutoTuneReason::SensorAbort); return false; }
    checkTimeout(now);
    if (!running()) return false;
    if (std::fabs(cfg.targetTemp - target_) > 0.01f) { abort(AutoTuneReason::ModeAbort); return false; }
    maxPct_ = static_cast<float>(cfg.maxHeaterPower);
    switch (phase_) {
      case AutoTunePhase::Baseline: return baseline(now, pv);
      case AutoTunePhase::Excite: case AutoTunePhase::Excite2: return excite(now, pv, cfg);
      case AutoTunePhase::Coast: return coast(now, pv, cfg);
      case AutoTunePhase::Approach: return approach(now, pv, cfg);
      case AutoTunePhase::NearSp: return nearSp(now, pv, cfg);
      case AutoTunePhase::Settle: return settle(now, pv, cfg);
      case AutoTunePhase::Validating: return validating(now, pv, cfg, tunedOut);
      default: return false;
    }
  }

  // ---- queries --------------------------------------------------------------------------------------------
  AutoTuneState state() const { return state_; }
  AutoTunePhase phase() const { return phase_; }
  AutoTuneReason reason() const { return reason_; }
  AutoTuneReason rejection() const { return rejection_; }
  uint8_t progress() const { return progress_; }
  float power() const { return power_; }
  bool running() const { return state_ == AutoTuneState::Running; }
  // In VALIDATING the heater is driven by the normal controller running the candidate gains.
  bool externalControl() const { return running() && phase_ == AutoTunePhase::Validating; }
  void applyCandidate(MachineConfig &cfg) const {
    if (!candidateValid_) return;
    cfg.controlMode = ControlMode::Pid; cfg.kp = cand_.kp; cfg.ki = cand_.ki; cfg.kd = cand_.kd;
  }
  // True once when VALIDATING begins: the caller resets the PID/scheduler and projects the integral
  // from the measured mean duty (`holdPct`) so the candidate takes over without a heater step.
  bool takeValidationStart(float &holdPct) {
    if (!validationStartPending_) return false;
    validationStartPending_ = false; holdPct = model_.holdPct; return true;
  }
  uint32_t cycleSerial() const { return relay_.cycleSerial(); }
  uint32_t validationSerial() const { return validationSerial_; }
  uint32_t modelSerial() const { return modelSerial_; }
  const Cycle &lastCycle() const { return relay_.lastCycle(); }
  uint8_t cycleCount() const { return relay_.cycleCount(); }
  const Result &result() const { return relay_.result(); }
  AutoTuneModelView model() const {
    AutoTuneModelView v;
    v.gain = model_.gain; v.delaySec = model_.delaySec; v.coast100C = model_.coast100C; v.holdPct = model_.holdPct;
    v.ku = model_.ku; v.periodSec = model_.periodSec; v.confidence = model_.confidence;
    return v;
  }
  const SmartTune::Model &fullModel() const { return model_; }
  const Eval &evalFull() const { return eval_; }
  AutoTuneEvalView eval() const {
    AutoTuneEvalView v; v.overshoot = eval_.overshoot; v.mae = eval_.mae; v.p95 = eval_.p95; v.ripple = eval_.ripple;
    return v;
  }
  float candidateKp() const { return cand_.kp; }
  float candidateKi() const { return cand_.ki; }
  float candidateKd() const { return cand_.kd; }
  float relayHigh() const { return relayHigh_; }
  // Startup/braking knowledge for the closed-loop check: the same numbers AdaptiveV1::update() derives from a profile
  // of this confidence, so the candidate is validated under the braking it will actually run with once accepted.
  MayapThermal::StartupHint startupHint(float highC, float sp) const {
    MayapThermal::StartupHint h;
    if (!model_.valid) return h;
    auto ramp = [](float x, float lo, float hi) { return x <= lo ? 0.0f : x >= hi ? 1.0f : (x - lo) / (hi - lo); };
    const float conf = static_cast<float>(model_.confidence);
    const float strength = ramp(conf, 45.0f, 75.0f);
    if (strength <= 0.0f) return h;
    const float headroom = std::isfinite(highC) ? std::max(0.1f, highC - sp) : 0.7f;
    const float coastFull = model_.gain * (model_.delaySec + 8.0f);
    const float headroomExtra = std::min(1.0f, coastFull / (8.0f * headroom));
    const float margin = (1.30f - 0.20f * ramp(conf, 60.0f, 90.0f)) + headroomExtra;
    h.valid = true; h.strength = strength;
    h.coastPerOnMs = model_.gain * 0.001f * margin;
    h.gainPerSec = model_.gain; h.delaySec = model_.delaySec; h.holdPct = model_.holdPct;
    // A 0.1 C probe makes the 10 s IIR slope spike on every step: the 90 s end-to-end slope is the robust one.
    if (slopeCount_ >= 5U) {
      const uint8_t newest = static_cast<uint8_t>((slopeHead_ + SlopeN - 1U) % SlopeN);
      const uint8_t older = static_cast<uint8_t>((slopeHead_ + SlopeN - 5U) % SlopeN);   // 4 steps = 80 s back
      h.slopeValid = true;
      h.slopePerSec = (scratch_.v.slopePv[newest] - scratch_.v.slopePv[older]) / (4.0f * SlopeStepSec);
    }
    return h;
  }
  // The PID assist Adaptive V1 will give a profile of this confidence once accepted (hold feed-forward, correction-authority limiter, Ki
  // stability ceiling: AdaptiveV1::update() formulas for a freshly MEASURED profile, hold trust 1). The closed-loop check runs with it, so
  // the candidate is validated in the exact control structure it will run in - not as a bare PID.
  MayapThermal::Assist assist() const {
    MayapThermal::Assist a;
    if (!model_.valid) return a;
    auto ramp = [](float x, float lo, float hi) { return x <= lo ? 0.0f : x >= hi ? 1.0f : (x - lo) / (hi - lo); };
    const float conf = static_cast<float>(model_.confidence);
    float hold = 0.0f;
    if (model_.holdPct >= 3.0f /* Policy::HoldFfMinPct */) hold = model_.holdPct * 0.95f * ramp(conf, 30.0f, 60.0f);
    a.feedForward = hold;
    const float theta = std::max(3.0f, model_.delaySec);
    if (hold > 0.0f) {
      const float a0 = std::min(60.0f, std::max(5.0f, 10.0f / (std::max(model_.gain, 0.0005f) * theta)));
      const float w = ramp(conf, 60.0f, 80.0f);
      if (w > 0.0f) a.corrBase = a0 + (1.0f - w) * 100.0f;
    }
    const float kiLimit = 100.0f / (16.0f * std::max(model_.gain, 0.0005f) * theta * theta);
    const float w = ramp(conf, 45.0f, 75.0f);
    if (w > 0.0f) a.kiMax = kiLimit + (1.0f - w) * 1000.0f;
    return a;
  }
  bool hasProfile() const { return state_ == AutoTuneState::Success && model_.valid; }
  // Measured plant as a ThermalProfile seed (signature/epoch/sequence are the caller's).
  MayapThermal::ThermalProfile profile() const {
    MayapThermal::ThermalProfile p = MayapThermal::defaultProfile();
    p.heaterGain = model_.gain; p.heaterDelaySec = model_.delaySec;
    p.coastRiseC = model_.coast100C; p.coastTimeSec = model_.coastSec;
    p.holdPowerPct = model_.holdPct; p.ventCoolingGain = 0.0f;
    p.confidence = model_.confidence; p.ventConfidence = 0;
    p.state = static_cast<uint8_t>(MayapThermal::LearnState::Learning);
    MayapThermal::sanitizeProfile(p);
    return p;
  }

 private:
  struct Candidate { float kp = 0, ki = 0, kd = 0; };
  static constexpr float sampleSec = 2.0f;

  void enter(AutoTunePhase p, uint32_t now) { phase_ = p; phaseAt_ = now; }
  float exciteOnSec() const { return (onMs_ - onAtExcite_) * 0.001f; }

  // ------------------------------------------------------------------ BASELINE
  bool baseline(uint32_t now, float pv) {
    power_ = 0; progress_ = 3;
    const float jump = std::fabs(pv - basePrev_);
    if (jump > baseMaxJump_) baseMaxJump_ = jump;
    const float d = pv - basePrev_;
    baseSq_ += d * d; ++baseN_; basePrev_ = pv; baseSum_ += pv;
    if (elapsedMs(now, phaseAt_) < SmartTune::BaselineMs) return false;
    const float spanS = std::max(1.0f, elapsedMs(now, baseFirstAt_) * 0.001f);
    const float slope = (pv - baseFirst_) / spanS;
    const float std_ = baseN_ > 1 ? std::sqrt(baseSq_ / static_cast<float>(baseN_) * 0.5f) : 0.0f;
    if (baseMaxJump_ > SmartTune::BaselineMaxJump || std::fabs(slope) > SmartTune::BaselineMaxSlope ||
        std_ > SmartTune::BaselineMaxStd) { abort(AutoTuneReason::BaselineUnstable); return false; }
    pvBase_ = baseSum_ / static_cast<float>(baseN_);
    startExcite(now, pv, SmartTune::ExcitePct, AutoTunePhase::Excite);
    return false;
  }
  void startExcite(uint32_t now, float pv, float pct, AutoTunePhase ph) {
    excitePct_ = pct; enter(ph, now);
    onAtExcite_ = onMs_;
    exDuty_ = std::min(excitePct_, maxPct_) * 0.01f;
    scratch_.est.reset(ph == AutoTunePhase::Excite ? pvBase_ : pv, now, exDuty_);
    pvExciteStart_ = pv; progress_ = ph == AutoTunePhase::Excite ? 6 : 36;
  }

  // ------------------------------------------------------------------ EXCITE
  bool excite(uint32_t now, float pv, const MachineConfig &cfg) {
    power_ = std::min(excitePct_, static_cast<float>(cfg.maxHeaterPower));
    if (power_ < 5.0f) { abort(AutoTuneReason::PowerLimited); return false; }
    scratch_.est.sample(now, pv);
    const uint32_t el = elapsedMs(now, phaseAt_);
    progress_ = static_cast<uint8_t>(6 + std::min<uint32_t>(18U, el * 18U / SmartTune::ExciteMaxMs));
    const float rise = pv - pvExciteStart_;
    const float headroom = target_ - pvExciteStart_;
    const float cap = std::min(SmartTune::RiseCapC, SmartTune::RiseCapHeadroom * headroom);
    if (target_ - pv < 1.0f) { abort(AutoTuneReason::NoHeadroom); return false; }
    bool stop = rise >= cap || el >= SmartTune::ExciteMaxMs;
    if (!stop && el >= SmartTune::ExciteMinMs && rise >= SmartTune::MinRiseC) {
      const SmartTune::StepEstimator::Fit f = scratch_.est.fit();
      // The ramp is only a ramp after the lag has died out (~3 time constants after the dead time): keep exciting until the
      // data covers that, or the lag is mistaken for extra delay.
      stop = f.valid && f.relSe <= SmartTune::GoodRelSe && f.separation >= SmartTune::GoodSeparation &&
             el >= static_cast<uint32_t>(SmartTune::RampCoversDelays * f.delay * 1000.0f);
    }
    if (!stop) return false;
    // cut: the coast of this excitation is measured next
    pvOff_ = pvPeak_ = pv; peakAt_ = now;
    exFit_ = scratch_.est.fit();
    exOnSec_ = exciteOnSec(); exDurSec_ = static_cast<float>(el) * 0.001f;
    enter(AutoTunePhase::Coast, now);
    power_ = 0; progress_ = static_cast<uint8_t>(phase_ == AutoTunePhase::Coast ? (used2_ ? 44 : 28) : 28);
    return false;
  }

  // ------------------------------------------------------------------ COAST
  bool coast(uint32_t now, float pv, const MachineConfig &cfg) {
    power_ = 0;
    if (pv > pvPeak_) { pvPeak_ = pv; peakAt_ = now; }
    if (target_ - pv < 0.8f) { abort(AutoTuneReason::NoHeadroom); return false; }
    const uint32_t el = elapsedMs(now, phaseAt_);
    const float theta = exFit_.valid ? exFit_.delay : 60.0f;
    const uint32_t minMs = static_cast<uint32_t>((1.2f * theta + 30.0f) * 1000.0f);
    const bool plateau = el >= minMs && elapsedMs(now, peakAt_) >= SmartTune::CoastPlateauMs;
    if (!plateau && el < SmartTune::CoastMaxMs) return false;
    // ---- evaluate the identification
    // The regression used the COMMANDED duty; the gain is rescaled by the ACTUAL / commanded ratio measured over the excitation.
    const float actualDuty = exDurSec_ > 1.0f ? exOnSec_ / exDurSec_ : 0.0f;
    const float ratioDuty = exDuty_ > 1e-3f ? actualDuty / exDuty_ : 0.0f;
    SmartTune::StepEstimator::Fit f = exFit_;
    if (ratioDuty < 0.5f || ratioDuty > 1.6f) f.valid = false;      // the arbiter delivered something else than commanded
    else f.gain = f.gain / ratioDuty;
    const bool good = f.valid && f.relSe <= SmartTune::GoodRelSe * 1.5f && f.separation >= SmartTune::GoodSeparation &&
                      f.rise >= SmartTune::MinRiseC && f.gain >= MayapThermal::Limits::GainMin && f.gain <= MayapThermal::Limits::GainMax;
    const float coastRise = std::max(0.0f, pvPeak_ - pvOff_);
    const float theta1 = f.valid ? f.delay : 0.0f;
    const float duty = std::max(0.02f, std::min(1.0f, actualDuty));
    const float predicted = f.gain * duty * theta1;
    const float ratio = predicted > 1e-4f ? coastRise / predicted : 0.0f;
    const bool consistent = predicted > 0.05f ? (ratio >= SmartTune::CoastRatioMin && ratio <= SmartTune::CoastRatioMax) : true;
    if (!(good && consistent)) {
      if (!used2_ && f.valid && f.gain > 0.0f && target_ - pv >= 2.0f &&
          static_cast<float>(cfg.maxHeaterPower) > SmartTune::ExcitePct + 5.0f) {
        used2_ = true;   // ONE more, stronger (hard-capped) excitation; never an escalation loop
        startExcite(now, pv, SmartTune::Excite2Pct, AutoTunePhase::Excite2);
        return false;
      }
      rejection_ = AutoTuneReason::ModelInvalid;
      abort(f.valid ? AutoTuneReason::ModelInvalid : AutoTuneReason::NoResponse);
      return false;
    }
    model_.gain = f.gain; model_.delaySec = f.delay; model_.relSe = f.relSe; model_.separation = f.separation;
    model_.coastRiseC = coastRise; model_.coastRatio = ratio;
    model_.coast100C = std::min(MayapThermal::Limits::CoastRiseMax, coastRise / std::max(0.05f, duty));
    model_.coastSec = std::min(MayapThermal::Limits::CoastTimeMax, elapsedMs(peakAt_, phaseAt_) * 0.001f);
    ++modelSerial_;
    std::memset(&scratch_.v, 0, sizeof(scratch_.v));
    slopeCount_ = 0; slopeHead_ = 0; nearAt_ = 0;
    enter(AutoTunePhase::Approach, now);
    progress_ = 40;
    return false;
  }

  // ------------------------------------------------------------------ APPROACH
  // Prediction brake  u = 40 (SP - 0.6 - PV) / coast100  (the coast of u % stays well under SP - 0.6). With the plant's loss
  // it settles where u equals the duty needed to stand still: that equilibrium is MEASURED (mean actual duty) and sizes the relay.
  bool approach(uint32_t now, float pv, const MachineConfig &cfg) {
    if (pv >= cfg.highTempAlarm - 0.3f) { abort(AutoTuneReason::SafetyAbort); return false; }
    const float coast100 = std::max(0.05f, model_.coast100C);
    // Loop gain of the brake alone is Kh*G*theta/100 = 100/G_pct... G = ApproachGain/coast100 keeps it at 0.4 (no ringing through the delay).
    const float u = SmartTune::ApproachGain * (target_ - SmartTune::ApproachBrakeC - pv) / coast100;
    // Never command more than what would still coast under the High alarm (in-flight heat cannot be recalled).
    const float uSafe = 100.0f * 0.6f * (cfg.highTempAlarm - 0.25f - pv) / coast100;
    power_ = std::max(0.0f, std::min(std::min(static_cast<float>(cfg.maxHeaterPower), u), uSafe));
    const float span = std::max(1.0f, target_ - pvBase_);
    progress_ = static_cast<uint8_t>(40 + std::min(18.0f, 18.0f * std::max(0.0f, pv - pvBase_) / span));
    pushSlope(now, pv);
    if (nearAt_ == 0U) nearAt_ = now ? now : 1U;   // approach start: the 200 s ring only holds approach samples
    if (slopeCount_ < SlopeN || elapsedMs(now, nearAt_) < 200000UL + static_cast<uint32_t>(3.0f * model_.delaySec * 1000.0f)) return false;
    const uint8_t newest = static_cast<uint8_t>((slopeHead_ + SlopeN - 1U) % SlopeN), oldest = slopeHead_;
    if (std::fabs(scratch_.v.slopePv[newest] - scratch_.v.slopePv[oldest]) > SmartTune::ApproachArriveC) return false;
    // equilibrium: mean actual duty over the window, minus the residual drift expressed as duty
    const float winSec = (SlopeN - 1U) * SlopeStepSec;
    const float onSec = (scratch_.v.slopeOn[newest] - scratch_.v.slopeOn[oldest]) * 0.001f;
    float holdEq = 100.0f * onSec / winSec - 100.0f * ((scratch_.v.slopePv[newest] - scratch_.v.slopePv[oldest]) / winSec) / std::max(1e-4f, model_.gain);
    holdEq = std::max(0.0f, std::min(100.0f, holdEq));
    model_.holdEqPct = holdEq;
    // The hold grows with (T - ambient): scale the equilibrium duty to the setpoint, taking the cold baseline as ambient
    // (capped at x1.6; the unscaled value stays the SAFE lower bound that limits the relay's overshoot).
    float scale = 1.0f;
    if (pv - pvBase_ >= 1.0f) scale = std::min(1.6f, std::max(1.0f, (target_ - pvBase_) / (pv - pvBase_)));
    const float holdSp = std::min(100.0f, holdEq * scale);
    if (holdSp > SmartTune::HoldMaxPct) { abort(AutoTuneReason::PowerLimited); return false; }
    // relay power: above the hold, and the part above the hold must coast under the High alarm
    bandC_ = std::min(0.25f, std::max(0.05f, cfg.autotuneBandC));
    const float roomC = std::max(0.0f, (cfg.highTempAlarm - target_) - bandC_ - SmartTune::RelayCoastMarginC);
    float high = std::min(holdSp * 1.15f + 0.7f * roomC * 100.0f / coast100, holdEq + 0.9f * roomC * 100.0f / coast100);
    high = std::min(std::min(60.0f, static_cast<float>(cfg.maxHeaterPower)), high);
    if (!(high >= 5.0f) || high < holdSp * 1.1f + 2.0f) { abort(AutoTuneReason::PowerLimited); return false; }
    relayHigh_ = std::floor(high);
    startRelay(now, pv, cfg);
    return false;
  }

  void startRelay(uint32_t now, float pv, const MachineConfig &cfg) {
    (void)cfg;
    relay_.setPreheatPercent(static_cast<uint8_t>(relayHigh_));
    relay_.setLimits(4800000UL, 1800000UL, 1800000UL);
    relay_.configure(target_);
    relay_.start(now, pv);
    relayRunning_ = true;
    enter(AutoTunePhase::NearSp, now);
    progress_ = 60;
  }

  // ------------------------------------------------------------------ NEAR SETPOINT (relay reused)
  bool nearSp(uint32_t now, float pv, const MachineConfig &cfg) {
    MachineConfig rc = cfg;
    rc.autotuneRelayPowerPercent = static_cast<uint8_t>(relayHigh_);
    rc.autotuneBandC = bandC_;
    MachineConfig scratch = rc;
    (void)relay_.update(now, pv, rc, scratch);   // the relay's own Tyreus candidate is discarded
    power_ = relay_.power();
    progress_ = static_cast<uint8_t>(60 + std::min<uint32_t>(25U, relay_.cycleCount() * 8U));
    if (relay_.state() == AutoTuneState::Failed) {
      rejection_ = relay_.reason();   // why the relay gave up (kept for diagnostics)
      abort(relay_.reason() == AutoTuneReason::SafetyAbort ? AutoTuneReason::SafetyAbort : AutoTuneReason::RelayFailed);
      return false;
    }
    if (relay_.state() != AutoTuneState::Success) return false;
    relayRunning_ = false;
    const RelayAutoTune::Result &r = relay_.result();
    model_.ku = r.ku; model_.periodSec = r.periodSec;
    const float cyc = r.heatSec + r.coolSec;
    model_.holdPct = cyc > 1.0f ? std::min(100.0f, std::max(0.0f, relayHigh_ * r.heatSec / cyc)) : 0.0f;
    power_ = 0;
    if (model_.holdPct > SmartTune::HoldMaxPct) { abort(AutoTuneReason::PowerLimited); return false; }
    enter(AutoTunePhase::Settle, now);
    progress_ = 86;
    return false;
  }

  // ------------------------------------------------------------------ SETTLE (cool to the setpoint, heater off)
  bool settle(uint32_t now, float pv, const MachineConfig &cfg) {
    power_ = 0;
    // The check starts from a real approach error (the setpoint is approached from below, integral at zero), the situation in which
    // the startup braking and the controller's wind-up recovery matter. Plants that cannot cool that far in time start from where they are.
    if (pv > target_ - SmartTune::SettleBelowC && elapsedMs(now, phaseAt_) < SmartTune::SettleCoolMs) return false;
    if (pv > target_ + 0.05f) return false;
    return generate(now, pv, cfg);
  }

  // ------------------------------------------------------------------ GENERATE (model-based, conservative)
  bool generate(uint32_t now, float pv, const MachineConfig &cfg) {
    enter(AutoTunePhase::Generate, now);
    float kp = 0.0f, ki = 0.0f, tauI = 0.0f;
    if (!SmartTune::simc(model_.gain, model_.delaySec, model_.ku, model_.periodSec, kp, ki, tauI)) {
      rejection_ = AutoTuneReason::CandidateInvalid; abort(AutoTuneReason::CandidateInvalid); return false;
    }
    MachineConfig c = cfg;
    c.controlMode = ControlMode::Pid; c.kp = kp; c.ki = ki; c.kd = 0.0f;
    const float p = c.kp, i = c.ki;
    sanitizeMachineConfig(c);
    if (c.kp != p || c.ki != i || c.kd != 0.0f || !std::isfinite(c.kp + c.ki)) {
      rejection_ = AutoTuneReason::CandidateInvalid; abort(AutoTuneReason::CandidateInvalid); return false;
    }
    cand_.kp = c.kp; cand_.ki = c.ki; cand_.kd = 0.0f; candidateValid_ = true; model_.tauI = tauI;
    model_.valid = true;
    // confidence of the ACTIVE measurement: higher than a default seed, never 100 %
    const float q1 = std::max(0.0f, std::min(1.0f, (0.12f - model_.relSe) / 0.12f));
    const float q2 = 1.0f - std::min(1.0f, std::fabs(model_.coastRatio - 1.0f) / 0.6f);
    const float q3 = model_.ku > 0.0f ? std::min(1.0f, (model_.ku / std::max(1e-3f, kp) - SmartTune::KuMargin + 1.0f) / 2.0f) : 0.5f;
    model_.confidence = static_cast<uint8_t>(SmartTune::ConfBase + SmartTune::ConfSpan * (q1 + q2 + q3) / 3.0f);
    ++modelSerial_;
    // ---- start the closed-loop check
    {
      float tail = SmartTune::ValidateTailPeriods * std::max(0.0f, model_.periodSec) * 1000.0f;
      tail = std::max(static_cast<float>(SmartTune::ValidateTailMs), std::min(static_cast<float>(SmartTune::ValidateMaxTailMs), tail));
      tailMs_ = static_cast<uint32_t>(tail);
      validateMs_ = std::max<uint32_t>(SmartTune::ValidateMs, 1800000UL + tailMs_);
    }
    ++validationSerial_;
    validationStartPending_ = true;
    enter(AutoTunePhase::Validating, now);
    eval_ = Eval(); eval_.ran = true;
    integralPeak_ = 0; slopeCount_ = 0; slopeHead_ = 0; for (uint8_t k = 0; k < HistN; ++k) scratch_.v.bins[k] = 0;
    tailMin_ = 1e9f; tailMax_ = -1e9f; tailSat_ = 0; tailN_ = 0; tailAbs_ = 0;
    maxOver_ = -1e9f;
    progress_ = 88;
    (void)pv;
    return false;
  }

  // ------------------------------------------------------------------ VALIDATING (closed loop, candidate gains)
  bool validating(uint32_t now, float pv, const MachineConfig &cfg, MachineConfig &tunedOut) {
    power_ = 0;   // the normal controller owns the heater
    const uint32_t el = elapsedMs(now, phaseAt_);
    pushSlope(now, pv);
    const float e = pv - target_;
    if (e > maxOver_) maxOver_ = e;
    if (maxOver_ > SmartTune::ValidOvershootC) return failValidation(AutoTuneReason::ValidationFailed, true);
    if (pv >= cfg.highTempAlarm - 0.15f) return failValidation(AutoTuneReason::ValidationFailed, true);
    progress_ = static_cast<uint8_t>(88 + std::min<uint32_t>(11U, el * 11U / validateMs_));
    if (el + tailMs_ >= validateMs_) {
      ++tailN_;
      const float a = std::fabs(e);
      tailAbs_ += a;
      tailMin_ = std::min(tailMin_, pv); tailMax_ = std::max(tailMax_, pv);
      uint8_t b = static_cast<uint8_t>(std::min<float>(HistN - 1U, a * 100.0f));
      ++scratch_.v.bins[b];
      if (reqPct_ >= static_cast<float>(cfg.maxHeaterPower) - 0.5f) ++tailSat_;
      if (std::fabs(integral_) > integralPeak_) integralPeak_ = std::fabs(integral_);
    }
    if (el < validateMs_) return false;
    // ---- score
    eval_.samples = tailN_;
    eval_.overshoot = maxOver_;
    eval_.mae = tailN_ ? tailAbs_ / static_cast<float>(tailN_) : 99.0f;
    uint32_t acc = 0; float p95 = 99.0f;
    const uint32_t need = (tailN_ * 95U + 99U) / 100U;
    for (uint8_t k = 0; k < HistN; ++k) { acc += scratch_.v.bins[k]; if (acc >= need) { p95 = (k + 1U) * 0.01f; break; } }
    eval_.p95 = p95;
    eval_.ripple = tailN_ ? tailMax_ - tailMin_ : 99.0f;
    eval_.satFrac = tailN_ ? static_cast<float>(tailSat_) / static_cast<float>(tailN_) : 1.0f;
    eval_.integralPeak = integralPeak_;
    const float maxOut = static_cast<float>(cfg.maxHeaterPower);
    eval_.pass = tailN_ >= 100U && eval_.overshoot <= SmartTune::ValidOvershootC && eval_.mae <= SmartTune::ValidMae &&
                 eval_.p95 <= SmartTune::ValidP95 && eval_.ripple <= SmartTune::ValidRipple &&
                 eval_.satFrac <= SmartTune::ValidSatFrac && eval_.integralPeak < 0.95f * maxOut;
    if (!eval_.pass) return failValidation(AutoTuneReason::ValidationFailed, false);
    tunedOut = cfg;
    applyCandidate(tunedOut);
    state_ = AutoTuneState::Success; phase_ = AutoTunePhase::Success; reason_ = AutoTuneReason::Success;
    power_ = 0; progress_ = 100;
    return true;
  }
  bool failValidation(AutoTuneReason why, bool immediate) {
    (void)immediate;
    rejection_ = why;
    abort(why);
    return false;
  }

  // The identification regression (EXCITE..COAST) and the approach / validation scratch (APPROACH..VALIDATING) never live at the same
  // time, so they share one block of RAM.
  union Scratch {
    Scratch() : est() {}
    SmartTune::StepEstimator est;
    struct V { float slopePv[11]; uint32_t slopeOn[11]; uint16_t bins[25]; } v;
  } scratch_;
  static constexpr uint8_t HistN = 25;   // 0.01 C bins up to 0.24 C (the P95 limit is 0.18)
  static constexpr uint8_t SlopeN = 11;   // 11 entries every 20 s = a 200 s window
  static constexpr float SlopeStepSec = 20.0f;
  uint8_t slopeHead_ = 0, slopeCount_ = 0;
  uint32_t slopeAt_ = 0, nearAt_ = 0;
  void pushSlope(uint32_t now, float pv) {
    if (slopeCount_ != 0U && elapsedMs(now, slopeAt_) < static_cast<uint32_t>(SlopeStepSec * 1000.0f)) return;
    slopeAt_ = now; scratch_.v.slopePv[slopeHead_] = pv; scratch_.v.slopeOn[slopeHead_] = onMs_;
    slopeHead_ = static_cast<uint8_t>((slopeHead_ + 1U) % SlopeN);
    if (slopeCount_ < SlopeN) ++slopeCount_;
  }

  AutoTuneState state_ = AutoTuneState::Idle;
  AutoTunePhase phase_ = AutoTunePhase::Idle;
  AutoTuneReason reason_ = AutoTuneReason::None, rejection_ = AutoTuneReason::None;
  float target_ = 37.5f, power_ = 0, excitePct_ = 25.0f, relayHigh_ = 0, bandC_ = 0.20f;
  uint32_t startedAt_ = 0, phaseAt_ = 0, peakAt_ = 0;
  uint32_t onMs_ = 0, onAtExcite_ = 0, tickAt_ = 0;
  bool tickOn_ = false, tickSeen_ = false, used2_ = false, relayRunning_ = false, candidateValid_ = false;
  bool validationStartPending_ = false;
  uint32_t validationSerial_ = 0, modelSerial_ = 0;
  uint8_t progress_ = 0;
  // baseline
  uint32_t baseN_ = 0, baseFirstAt_ = 0;
  float baseSum_ = 0, baseSq_ = 0, basePrev_ = 0, baseFirst_ = 0, baseMaxJump_ = 0, pvBase_ = 0;
  // identification
  SmartTune::StepEstimator::Fit exFit_;
  float pvExciteStart_ = 0, pvOff_ = 0, pvPeak_ = 0, exDuty_ = 0.25f, exOnSec_ = 0, exDurSec_ = 1, maxPct_ = 100;
  SmartTune::Model model_;
  Candidate cand_;
  RelayAutoTune relay_;
  // validation
  Eval eval_;
  float tailMin_ = 0, tailMax_ = 0, tailAbs_ = 0, maxOver_ = 0, integralPeak_ = 0, reqPct_ = 0, integral_ = 0;
  uint32_t tailN_ = 0, tailSat_ = 0, tailMs_ = SmartTune::ValidateTailMs, validateMs_ = SmartTune::ValidateMs;
};

// The engine the controller drives. Host tests define MAYAP_AUTOTUNE_LEGACY to run the original relay-only tune.
#ifdef MAYAP_AUTOTUNE_LEGACY
using AutoTuneEngine = RelayAutoTune;
#else
using AutoTuneEngine = SmartAutoTune;
#endif
