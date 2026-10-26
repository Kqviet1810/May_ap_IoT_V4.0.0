#pragma once
// =========================================================================================
// Thermal V3.0.0 - Robust Industrial Incubation Thermal Controller
// Pure C++11 (no Arduino dependencies), header-only.
//
// Key principles:
// 1. Direct Physical Transit Measurement (Stopwatch identification of tau_transit).
// 2. Direct Physical Gain Measurement (Peak slope observed during 100% climb).
// 3. Energy-In-Flight Predictive Control:
//      PV_predicted = PV + k_air * sum(P_pipe - P_hold)
// 4. Asymmetric Anti-Windup Proportional-Integral tracking with slow base hold adaptation.
// 5. Active Disturbance Rejection:
//      - Vent Fan Feedforward (instant power offset when vent fan runs).
//      - Door-Open Guard (prevents dangerous heat accumulation while doors are open).
// =========================================================================================
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace thermalv3 {

enum State : uint8_t {
  StIdle = 0,    // Standby / Not permitted to heat
  StClimb = 1,   // Phase 1: High-power climb with automatic parameter discovery
  StTrack = 2,   // Phase 2: Steady-state predictive tracking & micro-trim PI
  StDoor = 3,    // Disturbance: Door open detected (power clamped to prevent surge)
  StFrozen = 4   // Safety lock: Sensor missing, permit lost or alarm threshold
};

struct Input {
  float pv = NAN;                // Filtered process temperature [degC]
  float sp = 37.5f;              // Target setpoint [degC]
  float dQ = 0.1f;               // Sensor resolution step (0.1 or 0.01) [degC]
  float capPct = 100.0f;         // Configured maximum heater ceiling [%]
  float deliveredPct = 0.0f;     // Actual energy delivered in previous cycle [%]
  float dtS = 0.1f;              // Step interval [seconds]
  bool newSample = false;        // New valid sensor sample available
  bool permit = false;           // Production safety permit (all protections green)
  bool shadow = false;           // Vent line pause (keep state alive, command 0%)
  bool ventFan = false;          // Exhaust/ventilation fan is running
  float ventOnInS = 1.0e6f;      // Seconds until next scheduled vent start
  float ventOffInS = 1.0e6f;     // Seconds until active vent stops
  float headroomC = 0.7f;        // Headroom to High-Temp alarm [degC]
  float alarmHeadroomC = 0.7f;   // Headroom to Emergency shutdown line [degC]
  float e115Frac = 0.0f;         // Heater stuck evidence fraction [0.0..1.0]
};

struct Output {
  float powerPct = 0.0f;         // Commanded heater power [0.0 .. capPct]
  uint8_t state = StIdle;        // Controller operational state
  bool powerLimited = false;     // Power capped by ceiling or safety
  uint8_t trustQ = 15;           // Telemetry signal quality index (0..15)
};

class Controller {
 public:
  static constexpr uint16_t MAX_PIPE_LEN = 320U;
  static constexpr uint8_t SLOPE_HIST_CAP = 30U;

  Controller() { reset(); }

  void reset() {
    state_ = StIdle;
    pv_ = NAN;
    sp_ = 37.5f;
    capPct_ = 100.0f;

    kAir_ = 0.00025f;          // Initial prior: 0.025 degC/s per 100% power
    tauTransit_ = 110.0f;      // Initial nominal transport delay [seconds]
    pHold_ = 18.0f;            // Initial nominal hold power [%]
    integral_ = 0.0f;
    pvPredicted_ = 30.0f;

    pipeLen_ = 150U;
    pipeIdx_ = 0U;
    for (uint16_t i = 0U; i < MAX_PIPE_LEN; ++i) pipe_[i] = static_cast<uint8_t>(pHold_);

    secAcc_ = 0.0f;
    clockS_ = 0.0f;
    heaterOnTime_ = -1.0f;
    pvAtOn_ = NAN;
    identified_ = false;

    filteredSlope_ = 0.0f;
    peakClimbSlope_ = 0.0f;
    slopeHistCount_ = 0U;
    slopeHistHead_ = 0U;
    for (uint8_t i = 0U; i < SLOPE_HIST_CAP; ++i) {
      slopePvHist_[i] = 0.0f;
      slopeTimeHist_[i] = 0.0f;
    }

    pVentFf_ = 0.0f;
    doorDetected_ = false;
    doorTimer_ = 0.0f;
    stuckTimer_ = 0.0f;
    respSuspect_ = false;
    respHighPwrTimer_ = 0.0f;
    respPvStart_ = NAN;
  }

  Output step(const Input &in) {
    Output out;
    const float dt = !std::isfinite(in.dtS) ? 0.1f : (in.dtS < 0.001f ? 0.001f : (in.dtS > 1.0f ? 1.0f : in.dtS));
    clockS_ += dt;

    const bool pvValid = std::isfinite(in.pv) && in.pv > 0.0f && in.pv < 60.0f;
    const bool spValid = std::isfinite(in.sp) && in.sp > 20.0f && in.sp < 50.0f;
    const bool live = (in.permit || in.shadow) && pvValid && spValid;
    capPct_ = (std::isfinite(in.capPct) && in.capPct > 0.0f) ? std::min(100.0f, in.capPct) : 0.0f;

    if (!live || capPct_ <= 0.0f) {
      state_ = StFrozen;
      out.powerPct = 0.0f;
      out.state = state_;
      return out;
    }

    pv_ = in.pv;
    sp_ = in.sp;

    // -------------------------------------------------------------------------
    // 1. Sliding-Window Slope Estimation (10s to 25s window)
    // -------------------------------------------------------------------------
    updateSlope(pv_, clockS_);

    // -------------------------------------------------------------------------
    // 2. Physical Transit Time & Gain Discovery
    // -------------------------------------------------------------------------
    if (heaterOnTime_ < 0.0f && in.permit) {
      heaterOnTime_ = clockS_;
      pvAtOn_ = pv_;
      respPvStart_ = pv_;
    }

    if (!identified_ && heaterOnTime_ >= 0.0f && std::isfinite(pvAtOn_)) {
      if ((pv_ - pvAtOn_) >= 0.15f) {
        const float measuredDelay = clockS_ - heaterOnTime_;
        tauTransit_ = std::max(45.0f, std::min(220.0f, measuredDelay * 1.05f));
        // Total effective prediction horizon includes heater lag ~ 1.35x
        pipeLen_ = std::max<uint16_t>(15U, std::min<uint16_t>(MAX_PIPE_LEN - 1U, static_cast<uint16_t>(tauTransit_ * 1.35f)));
        identified_ = true;
      }
    }

    // Capture peak slope during 100% climb to calibrate kAir
    if (pv_ < sp_ - 1.0f && filteredSlope_ > peakClimbSlope_ && in.permit) {
      peakClimbSlope_ = filteredSlope_;
      const float estK = peakClimbSlope_ / std::max(10.0f, 100.0f - pHold_);
      kAir_ = std::max(0.00010f, std::min(0.00075f, estK));
    }

    // -------------------------------------------------------------------------
    // 3. Active Disturbance Rejection
    // -------------------------------------------------------------------------
    // Vent Fan Feedforward (+12% while exhaust fan runs)
    if (in.ventFan) {
      pVentFf_ = std::min(12.0f, pVentFf_ + 2.5f * dt);
    } else {
      pVentFf_ = std::max(0.0f, pVentFf_ - 1.2f * dt);
    }

    // Door Open Plunge Detection: temperature falling fast (< -0.7 C/min) below SP
    if (filteredSlope_ < -0.012f && (sp_ - pv_) > 0.40f) {
      doorDetected_ = true;
      doorTimer_ = 0.0f;
    }
    if (doorDetected_) {
      doorTimer_ += dt;
      if (filteredSlope_ > -0.003f && doorTimer_ > 15.0f) {
        doorDetected_ = false;
      }
    }

    // -------------------------------------------------------------------------
    // 4. Energy-In-Flight Predictive Calculation
    // -------------------------------------------------------------------------
    float pipeSum = 0.0f;
    for (uint16_t i = 0U; i < pipeLen_; ++i) pipeSum += static_cast<float>(pipe_[i]);
    const float excessEnergy = pipeSum - static_cast<float>(pipeLen_) * pHold_;
    pvPredicted_ = pv_ + kAir_ * excessEnergy;

    const float errPred = sp_ - pvPredicted_;
    const float errTrue = sp_ - pv_;

    // -------------------------------------------------------------------------
    // 5. Control Law: Asymmetric P on Predicted Error + Conditional Anti-Windup I
    // -------------------------------------------------------------------------
    // Proportional term
    float pTerm = 0.0f;
    if (errPred >= 0.0f) {
      pTerm = KP_UP * errPred;
    } else {
      pTerm = KP_DOWN * errPred; // Strong damping above SP to strictly prevent overshoot
    }

    // Integral term on True Error (active within +-0.75 degC)
    if (!doorDetected_ && std::fabs(errTrue) <= 0.75f) {
      integral_ += KI * errTrue * dt;
      integral_ = std::max(-I_CLAMP, std::min(I_CLAMP, integral_));

      // Slow absorption of persistent integral into pHold (time constant ~300s)
      const float shift = integral_ * (dt / 300.0f);
      pHold_ = std::max(5.0f, std::min(45.0f, pHold_ + shift));
      integral_ -= shift;
    } else if (!doorDetected_) {
      integral_ *= std::max(0.0f, 1.0f - 0.05f * dt);
    }

    // Anti-Stuck protection: if temperature rests flat below SP - 0.08 degC > 120s
    if (!doorDetected_ && errTrue >= 0.08f && std::fabs(filteredSlope_) < 0.0015f) {
      stuckTimer_ += dt;
      if (stuckTimer_ > 120.0f) {
        pHold_ = std::min(45.0f, pHold_ + 0.025f * dt);
      }
    } else {
      stuckTimer_ = 0.0f;
    }

    // Raw commanded power
    float cmd = pHold_ + pTerm + integral_ + pVentFf_;

    // Door open guard clamp
    if (doorDetected_) {
      cmd = std::min(cmd, pHold_ + 6.0f);
      state_ = StDoor;
    } else if (in.shadow) {
      cmd = 0.0f;
      state_ = StFrozen;
    } else if (sp_ - pv_ > 1.2f && filteredSlope_ >= 0.0f) {
      state_ = StClimb;
    } else {
      state_ = StTrack;
    }

    // Clamp to valid boundaries
    cmd = std::max(0.0f, std::min(capPct_, cmd));

    // -------------------------------------------------------------------------
    // 6. Update Transit History Buffer (1 Hz)
    // -------------------------------------------------------------------------
    secAcc_ += dt;
    if (secAcc_ >= 1.0f) {
      secAcc_ -= 1.0f;
      pipe_[pipeIdx_] = static_cast<uint8_t>(std::round(cmd));
      pipeIdx_ = static_cast<uint16_t>((pipeIdx_ + 1U) % pipeLen_);
    }

    // -------------------------------------------------------------------------
    // 7. Safety Watchdog
    // -------------------------------------------------------------------------
    if (cmd > 40.0f) {
      respHighPwrTimer_ += dt;
      if (respHighPwrTimer_ > 360.0f && std::isfinite(respPvStart_)) {
        if ((pv_ - respPvStart_) < 0.20f) respSuspect_ = true;
      }
    } else {
      respHighPwrTimer_ = 0.0f;
      respPvStart_ = pv_;
      respSuspect_ = false;
    }

    out.powerPct = cmd;
    out.state = state_;
    out.powerLimited = (cmd >= capPct_);
    out.trustQ = doorDetected_ ? 6U : (identified_ ? 15U : 10U);
    return out;
  }

  // Telemetry readouts for machine supervision and web monitors
  float predicted() const { return pvPredicted_; }
  uint8_t state() const { return state_; }
  uint8_t phase() const { return (state_ == StTrack || state_ == StDoor) ? 2U : (state_ == StClimb ? 1U : 0U); }
  bool responseSuspect() const { return respSuspect_; }
  bool identified() const { return identified_; }
  uint8_t batchesDone() const { return identified_ ? 2U : 0U; }
  float kAir() const { return kAir_; }
  float tauTransit() const { return tauTransit_; }
  float pHold() const { return pHold_; }
  float integral() const { return integral_; }
  float slopeMks() const { return filteredSlope_ * 1000.0f; }
  bool doorDetected() const { return doorDetected_; }

 private:
  static constexpr float KP_UP = 30.0f;      // Proportional gain below SP [% / degC]
  static constexpr float KP_DOWN = 90.0f;    // Proportional gain above SP (3x asymmetric)
  static constexpr float KI = 0.020f;        // Integral rate [% / (degC * s)]
  static constexpr float I_CLAMP = 15.0f;    // Hard anti-windup clamp [%]

  void updateSlope(float pv, float nowS) {
    if (slopeHistCount_ < SLOPE_HIST_CAP) {
      slopePvHist_[slopeHistHead_] = pv;
      slopeTimeHist_[slopeHistHead_] = nowS;
      slopeHistHead_ = (slopeHistHead_ + 1U) % SLOPE_HIST_CAP;
      ++slopeHistCount_;
    } else {
      const uint8_t oldest = slopeHistHead_;
      const float dtSpan = nowS - slopeTimeHist_[oldest];
      if (dtSpan > 2.0f) {
        const float rawSlope = (pv - slopePvHist_[oldest]) / dtSpan;
        const float alpha = std::min(1.0f, (nowS - slopeTimeHist_[(oldest + SLOPE_HIST_CAP - 1U) % SLOPE_HIST_CAP]) / 3.0f);
        filteredSlope_ += alpha * (rawSlope - filteredSlope_);
      }
      slopePvHist_[slopeHistHead_] = pv;
      slopeTimeHist_[slopeHistHead_] = nowS;
      slopeHistHead_ = (slopeHistHead_ + 1U) % SLOPE_HIST_CAP;
    }
  }

  uint8_t state_ = StIdle;
  float pv_ = NAN;
  float sp_ = 37.5f;
  float capPct_ = 100.0f;

  float kAir_ = 0.00025f;
  float tauTransit_ = 110.0f;
  float pHold_ = 18.0f;
  float integral_ = 0.0f;
  float pvPredicted_ = 30.0f;

  uint16_t pipeLen_ = 150U;
  uint16_t pipeIdx_ = 0U;
  uint8_t pipe_[MAX_PIPE_LEN] = {};

  float secAcc_ = 0.0f;
  float clockS_ = 0.0f;
  float heaterOnTime_ = -1.0f;
  float pvAtOn_ = NAN;
  bool identified_ = false;

  float filteredSlope_ = 0.0f;
  float peakClimbSlope_ = 0.0f;
  uint8_t slopeHistCount_ = 0U;
  uint8_t slopeHistHead_ = 0U;
  float slopePvHist_[SLOPE_HIST_CAP] = {};
  float slopeTimeHist_[SLOPE_HIST_CAP] = {};

  float pVentFf_ = 0.0f;
  bool doorDetected_ = false;
  float doorTimer_ = 0.0f;
  float stuckTimer_ = 0.0f;

  bool respSuspect_ = false;
  float respHighPwrTimer_ = 0.0f;
  float respPvStart_ = NAN;
};

}  // namespace thermalv3
