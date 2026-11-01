#pragma once
// =========================================================================================
// Thermal V3 - Clean-Slate Industrial Incubation Thermal Controller
// Pure C++11 (no Arduino dependencies), header-only.
//
// Core Control Architecture:
// 1. Dead-Time Predictive Trajectory Control (Transit Pipe + Measured Velocity Coast):
//      Models in-flight heater energy over pipeLen_ = 1.35 * tauTransit_ and blends far-field
//      predictive lead/braking smoothly into calm narrow-band tracking near setpoint.
// 2. Bounded Energy-Balance Hold Learner + Post-Knee Plant Identification:
//      Measures tauTransit_ on clean cold starts and captures kAir_ strictly past the initial
//      curvature knee. Tracks thermal demand (ambient loss + cold egg absorption) with fast
//      downward relaxation (TAU_DOWN = 32 s) and conservative upward adaptation (cap 65%).
// 3. Smooth C0-Continuous Over-Setpoint Attenuation Envelope:
//      Eliminates both high-power overshoot above SP (never pumping heavy hold power at
//      SP + 0.1..0.15 C) and bang-bang relay limit cycles on long-deadtime cold-room ovens.
// 4. Protection-Cut Hold Shedding, Active Door-Open Plunge Guard & 64-bit Monotonic Clock.
// =========================================================================================
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace thermalv3 {

constexpr float KP_UP = 38.0f;               // Max far-field climb gain [% / degC]
constexpr float KP_DOWN = 55.0f;             // Base damping gain above SP [% / degC]
constexpr float KP_HOLD_MIN = 4.5f;          // Minimum steady-state tracking gain [% / degC]
constexpr float KP_HOLD_MAX = 30.0f;         // Maximum near-SP tracking gain [% / degC]
constexpr float KI = 0.025f;                 // Narrow-band integral rate [% / (degC * s)]
constexpr float I_CLAMP = 15.0f;             // Integral trim clamp [%]
constexpr float HOLD_COLD_INIT_PCT = 16.0f;  // Initial hold prior on deep cold start (> 3.0 C below SP) [%]
constexpr float HOLD_NEAR_INIT_PCT = 8.0f;   // Safe low hold prior when starting within 3.0 C of SP [%]
constexpr float HOLD_MAX_PCT = 85.0f;        // Hard upper ceiling of learned hold (matches thermal_learn.h) [%]
constexpr float OVER_SHED_KEEP = 0.60f;      // Fraction of pHold retained when protection trips >= 0.15 C over SP
constexpr float MODEL_NEAR_C = 0.30f;        // |error| <= this: pure measured tracking + brake-only model
constexpr float MODEL_FAR_C = 1.00f;         // |error| >= this: full predictive trajectory control
constexpr float KAIR_MIN = 5.0e-5f;          // Heater gain floor [degC/s per %]
constexpr float KAIR_CTRL_MIN = 1.4e-4f;     // Internal gain floor for kpHold sizing
constexpr float KAIR_MAX = 7.5e-4f;          // Heater gain ceiling [degC/s per %]
constexpr float TAU_MIN_S = 90.0f;           // Minimum physical transport delay (matches thermal_learn.h) [s]
constexpr float TAU_MAX_S = 220.0f;          // Maximum physical transport delay [s]
constexpr float IDENT_MIN_ERR_C = 2.0f;      // Minimum error required for clean start stopwatch [degC]
constexpr float IDENT_MAX_SLOPE = 0.002f;    // Max initial slope for clean start stopwatch [degC/s]
constexpr float DOOR_MAX_S = 300.0f;         // Max duration of single door-open power clamp [s]
constexpr float DOOR_REARM_SLOPE = -0.006f;  // Slope above which door detector re-arms [degC/s]
constexpr float LEARN_WINDOW_S = 220.0f;     // Energy-balance window [s]
constexpr float LEARN_MIN_S = 90.0f;         // Minimum calm window for energy balance [s]
constexpr float LEARN_TAU_UP_S = 125.0f;     // Upward hold adaptation time constant [s]
constexpr float LEARN_TAU_DOWN_S = 32.0f;    // Fast downward hold relaxation time constant [s]
constexpr float LEARN_STEP_UP_MAX = 0.45f;   // Max upward hold step per second [%]
constexpr float LEARN_STEP_DOWN_MAX = 1.8f;  // Max downward hold step per second [%]
constexpr float LEARN_FALL_MAX_PCT = 28.0f;  // Max lift above uMean during steep fall [%]
constexpr uint16_t MOMENTUM_WINDOW_S = 45U;  // Least-squares window for momentum brake [s]
constexpr float MOMENTUM_NOISE_STEPS = 1.15f;
constexpr uint16_t HIST_LEN = 512U;          // 1 Hz history buffer

enum State : uint8_t {
  StIdle = 0,    // Standby / Not permitted to heat
  StClimb = 1,   // High-power climb / recovery far below SP
  StTrack = 2,   // Steady-state tracking near SP
  StDoor = 3,    // Door-open plunge guard active
  StFrozen = 4   // Safety lock / permit off / overheat vent hold
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
  bool shadow = false;           // Vent-line / overheat hold (keep state alive, command 0%)
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

    kAir_ = 0.0004f;
    tauTransit_ = 150.0f;
    pipeLen_ = 202U;
    pHold_ = HOLD_COLD_INIT_PCT;
    integral_ = 0.0f;
    pvPredicted_ = 30.0f;

    pipeIdx_ = 0U;
    for (uint16_t i = 0U; i < MAX_PIPE_LEN; ++i) pipe_[i] = static_cast<uint8_t>(pHold_);
    recomputePipeSum();

    histHead_ = 0U;
    histCount_ = 0U;
    for (uint16_t i = 0U; i < HIST_LEN; ++i) {
      pvHist_[i] = 0.0f;
      uHist_[i] = 0U;
    }

    slopeShort_ = 0.0f;
    slopeMed_ = 0.0f;
    dQ_ = 0.1f;
    liveS_ = 0.0f;
    calmS_ = 0.0f;
    lastCmd_ = 0.0f;
    wasLive_ = false;
    startDone_ = false;
    ovenImported_ = false;
    holdImported_ = false;
    probing_ = false;
    probeT_ = 0.0f;
    probePv_ = NAN;
    probeStart_ = NAN;
    probeQuiet_ = 0U;

    secAcc_ = 0.0f;
    clockS_ = 0.0;
    heaterOnTime_ = -1.0;
    pvAtOn_ = NAN;
    identified_ = false;

    filteredSlope_ = 0.0f;
    peakClimbSlope_ = 0.0f;
    slopeHistCount_ = 0U;
    slopeHistHead_ = 0U;
    for (uint8_t i = 0U; i < SLOPE_HIST_CAP; ++i) {
      slopePvHist_[i] = 0.0f;
      slopeTimeHist_[i] = 0.0;
    }

    doorDetected_ = false;
    doorArmed_ = true;
    doorTimer_ = 0.0f;
    doorCalmS_ = 0.0f;
    stuckTimer_ = 0.0f;
    stuckAnchorPv_ = NAN;

    shadowS_ = 0.0f;
    shadowRefPv_ = NAN;
    shadowRefS_ = 0.0f;
    shadowFade_ = false;

    respSuspect_ = false;
    respHighPwrTimer_ = 0.0f;
    respPvStart_ = NAN;
  }

  Output step(const Input &in) {
    Output out;
    const float dt = !std::isfinite(in.dtS) ? 0.1f : (in.dtS < 0.001f ? 0.001f : (in.dtS > 1.0f ? 1.0f : in.dtS));
    clockS_ += static_cast<double>(dt);

    const float dQ = (std::isfinite(in.dQ) && in.dQ >= 0.005f && in.dQ <= 0.2f) ? in.dQ : 0.1f;
    dQ_ = dQ;

    const bool pvValid = std::isfinite(in.pv) && in.pv > 0.0f && in.pv < 60.0f;
    const bool spValid = std::isfinite(in.sp) && in.sp > 20.0f && in.sp < 50.0f;
    const bool live = (in.permit || in.shadow) && pvValid && spValid;
    capPct_ = (std::isfinite(in.capPct) && in.capPct > 0.0f) ? std::min(100.0f, in.capPct) : 0.0f;

    if (!live || capPct_ <= 0.0f) {
      state_ = StFrozen;
      out.powerPct = 0.0f;
      out.state = state_;

      // Protection cut while reading is valid: shed excess hold once on >= 0.15 C overshoot,
      // and fade hold if chamber stays warm on its own for 3-minute checkpoints.
      if (pvValid && spValid && capPct_ > 0.0f && !in.permit && !in.shadow) {
        if (wasLive_ && in.pv >= in.sp + 0.15f) {
          pHold_ *= OVER_SHED_KEEP;
          if (integral_ > 0.0f) integral_ = 0.0f;
        }
        fadeWhileHeldOff(in.pv, in.sp, dt);
      }

      wasLive_ = false;
      liveS_ = 0.0f;
      calmS_ = 0.0f;
      lastCmd_ = 0.0f;
      stuckTimer_ = 0.0f;
      stuckAnchorPv_ = NAN;
      if (!identified_) heaterOnTime_ = -1.0;

      secAcc_ += dt;
      while (secAcc_ >= 1.0f) {
        secAcc_ -= 1.0f;
        pushPipe(0U);
        pushHist(pvValid ? in.pv : (std::isfinite(pv_) ? pv_ : 0.0f), 0.0f);
      }
      return out;
    }

    wasLive_ = true;
    pv_ = in.pv;
    sp_ = in.sp;

    // Safe distance-aware initial hold selection on first live step (never overrides NVS importHold):
    // Starting within 3.0 C of SP (e.g. warm room or batch restart at 36.0 C) uses low 8% prior!
    if (!startDone_) {
      startDone_ = true;
      if (!holdImported_) {
        const float e0 = sp_ - pv_;
        if (e0 <= 3.0f) {
          pHold_ = HOLD_NEAR_INIT_PCT;
          rebuildPipe();
        }
        if (e0 <= 1.35f) {
          probing_ = true;
          probeT_ = 0.0f;
          probePv_ = probeStart_ = pv_;
          probeQuiet_ = 0U;
        }
      }
    }

    if (probing_) {
      probeT_ += dt;
      const float stepS = std::max(180.0f, tauTransit_);
      if (pv_ < sp_ - 0.15f && pv_ < probeStart_ - 0.15f) {
        if (pHold_ < 18.0f) {
          pHold_ = 18.0f;
          rebuildPipe();
        }
        probing_ = false;
      } else if (pv_ >= sp_ + 0.10f || probeT_ > 1800.0f || in.shadow || doorDetected_) {
        probing_ = false;
      } else if (probeT_ >= stepS) {
        probeT_ = 0.0f;
        if (pv_ < probePv_ - 0.04f || pv_ < sp_ - 0.15f) {
          if (pv_ < probePv_ - 0.04f || slopeShort_ <= 0.0f || (pv_ < sp_ - 0.35f && slopeShort_ < 0.0008f)) {
            pHold_ = std::min(HOLD_MAX_PCT, pHold_ + 4.0f);
            probeQuiet_ = 0U;
          } else {
            ++probeQuiet_;
          }
        } else {
          ++probeQuiet_;
        }
        probePv_ = pv_;
        if (probeQuiet_ >= 2U) probing_ = false;
      }
    }

    // -------------------------------------------------------------------------
    // 1. Sliding-Window Slope Estimation (64-bit monotonic clockS_)
    // -------------------------------------------------------------------------
    updateSlope(pv_, clockS_);

    // -------------------------------------------------------------------------
    // 2. Clean Start Transit Delay & Post-Knee Gain Identification
    // -------------------------------------------------------------------------
    if (!std::isfinite(respPvStart_)) respPvStart_ = pv_;

    if (!identified_) {
      if (heaterOnTime_ < 0.0) {
        if (in.permit && !in.shadow && slopeHistCount_ >= SLOPE_HIST_CAP &&
            std::fabs(filteredSlope_) <= IDENT_MAX_SLOPE &&
            (sp_ - pv_) >= IDENT_MIN_ERR_C && lastCmd_ >= 0.8f * capPct_) {
          heaterOnTime_ = clockS_;
          pvAtOn_ = pv_;
        }
      } else if (lastCmd_ < 0.4f * capPct_) {
        heaterOnTime_ = -1.0;
      } else if ((pv_ - pvAtOn_) >= 0.15f) {
        const float measuredDelay = static_cast<float>(clockS_ - heaterOnTime_);
        tauTransit_ = std::max(TAU_MIN_S, std::min(TAU_MAX_S, measuredDelay * 1.05f));
        pipeLen_ = std::max<uint16_t>(15U, std::min<uint16_t>(MAX_PIPE_LEN - 1U, static_cast<uint16_t>(tauTransit_ * 1.35f)));
        rebuildPipe();
        identified_ = true;
      }
    }

    // Capture peak climb slope strictly past the initial turn-on knee using clean 45s OLS slope
    if (identified_ && pv_ < sp_ - 1.0f && slopeShort_ > peakClimbSlope_ && in.permit && !in.shadow &&
        heaterOnTime_ >= 0.0 && (clockS_ - heaterOnTime_) >= static_cast<double>(tauTransit_ + 25.0f)) {
      const uint16_t delay = static_cast<uint16_t>(tauTransit_ + 0.5f);
      const float uWin = delayedPower(delay, MOMENTUM_WINDOW_S);
      if (uWin >= 0.9f * capPct_) {
        peakClimbSlope_ = slopeShort_;
        const float estK = peakClimbSlope_ / std::max(20.0f, uWin - std::min(pHold_, 25.0f));
        kAir_ = std::max(KAIR_MIN, std::min(KAIR_MAX, estK));
      }
    }

    // -------------------------------------------------------------------------
    // 3. Active Door-Open Plunge Guard
    // -------------------------------------------------------------------------
    if (filteredSlope_ > DOOR_REARM_SLOPE) doorCalmS_ += dt;
    else doorCalmS_ = 0.0f;
    if (!doorArmed_ && !doorDetected_ && doorCalmS_ >= 20.0f) doorArmed_ = true;
    if (doorArmed_ && filteredSlope_ < -0.012f && (sp_ - pv_) > 0.40f) {
      doorDetected_ = true;
      doorArmed_ = false;
      doorTimer_ = 0.0f;
    }
    if (doorDetected_) {
      doorTimer_ += dt;
      if ((filteredSlope_ > -0.003f && slopeShort_ > -0.0025f && doorTimer_ > 15.0f) || doorTimer_ > DOOR_MAX_S) {
        doorDetected_ = false;
      }
    }

    // -------------------------------------------------------------------------
    // 4. Predictive Energy-In-Flight & Velocity Momentum Brake
    // -------------------------------------------------------------------------
    const auto deadZone = [dQ](float e) {
      return e > 0.5f * dQ ? e - 0.5f * dQ : (e < -0.5f * dQ ? e + 0.5f * dQ : 0.0f);
    };

    const float excessEnergy = static_cast<float>(pipeSum_) - static_cast<float>(pipeLen_) * pHold_;
    pvPredicted_ = std::max(pv_ - 0.20f, pv_ + kAir_ * excessEnergy);

    const float errPred = sp_ - pvPredicted_;
    const float errTrue = sp_ - pv_;

    const float deltaWinPv = (histCount_ >= MOMENTUM_WINDOW_S) ? (pvAge(0U) - pvAge(MOMENTUM_WINDOW_S - 1U)) : 0.0f;
    const float noiseFade = std::max(0.0f, std::min(1.0f, (1.85f * dQ - deltaWinPv) / (0.75f * dQ)));
    const float noiseSlope = noiseFade * (MOMENTUM_NOISE_STEPS * dQ / static_cast<float>(MOMENTUM_WINDOW_S));
    const float posSlopeShort = std::max(0.0f, slopeShort_ - noiseSlope);

    const float deltaMedPv = (histCount_ >= 120U) ? (pvAge(0U) - pvAge(119U)) : 0.0f;
    const float medNoiseFade = std::max(0.0f, std::min(1.0f, (1.95f * dQ - deltaMedPv) / (0.80f * dQ)));
    const float medNoiseSlope = medNoiseFade * (1.05f * dQ / 120.0f);
    const float posSlopeMed = (deltaMedPv >= 1.05f * dQ) ? std::max(0.0f, slopeMed_ - medNoiseSlope) : 0.0f;
    const float nearSpFade = std::max(0.0f, std::min(1.0f, (0.30f - std::fabs(errTrue)) / 0.15f));
    const float posSlope = std::max(posSlopeShort, nearSpFade * std::min(0.0020f, posSlopeMed));

    const float errMeas = deadZone(errTrue);
    const float errMomentum = deadZone(errTrue - posSlope * tauTransit_);
    const float errModel = std::min(deadZone(errPred), errMomentum);

    // -------------------------------------------------------------------------
    // 5. Predictive-to-Calm Tracking Control Law
    // -------------------------------------------------------------------------
    const float wModel = std::max(0.0f, std::min(1.0f, (std::fabs(errTrue) - MODEL_NEAR_C) / (MODEL_FAR_C - MODEL_NEAR_C)));
    const float kpHold = std::max(KP_HOLD_MIN, std::min(KP_HOLD_MAX, 1.0f / (2.0f * kAir_ * static_cast<float>(pipeLen_))));
    const float kpFar = std::max(kpHold, std::min(KP_UP, 1.12f / (kAir_ * static_cast<float>(pipeLen_))));
    const float kp = wModel * kpFar + (1.0f - wModel) * kpHold;

    const float errCtl = wModel * errModel + (1.0f - wModel) * std::min(errMeas, errModel);
    float pTerm = errCtl >= 0.0f ? kp * errCtl : KP_DOWN * errCtl;
    if (identified_ && errTrue >= 0.0f && pHold_ <= 15.0f && excessEnergy <= 5.5f * static_cast<float>(pipeLen_) && pTerm < -1.0f) {
      pTerm = -1.0f;
    }

    if (in.shadow) {
      fadeWhileHeldOff(pv_, sp_, dt);
      stuckTimer_ = 0.0f;
    } else {
      shadowS_ = 0.0f;
      shadowFade_ = false;
    }

    // Conditional narrow-band PI + gentle above-SP excess hold bleed
    if (!in.shadow && !doorDetected_) {
      if (errTrue < -std::max(0.05f, 1.005f * dQ)) {
        integral_ = std::max(-I_CLAMP, integral_ + 2.0f * KI * errMeas * dt);
        const float bleed = std::max(-0.08f, 0.14f * errMeas) * dt;
        pHold_ = std::max(0.0f, pHold_ + bleed);
      } else if (std::fabs(errTrue) <= 0.75f) {
        const float kiEff = std::min(KI, kpHold / (4.0f * static_cast<float>(pipeLen_)));
        integral_ = std::max(-I_CLAMP, std::min(I_CLAMP, integral_ + kiEff * errMeas * dt));
        const float shift = integral_ * (dt / 260.0f);
        pHold_ = std::max(0.0f, std::min(HOLD_MAX_PCT, pHold_ + shift));
        integral_ -= shift;
      } else {
        integral_ *= std::max(0.0f, 1.0f - 0.05f * dt);
      }

      // Anchor-based anti-stall ramp: ONLY triggers if PV fails to rise by >= 1.15 sensor steps over 120 s
      // while below SP and not already at heater ceiling (never winds up during slow climbs or initial deadtime!).
      if (errTrue >= std::max(0.08f, 0.95f * dQ) && lastCmd_ < 0.95f * capPct_ && posSlope < 0.0008f && filteredSlope_ > -0.002f) {
        if (!std::isfinite(stuckAnchorPv_) || pv_ < stuckAnchorPv_ - 0.15f || pv_ >= stuckAnchorPv_ + std::max(0.115f, 1.15f * dQ)) {
          stuckAnchorPv_ = pv_;
          stuckTimer_ = 0.0f;
        } else {
          stuckTimer_ += dt;
          if (stuckTimer_ > 120.0f) {
            pHold_ = std::min(HOLD_MAX_PCT, pHold_ + 0.025f * dt);
          }
        }
      } else {
        stuckTimer_ = 0.0f;
        stuckAnchorPv_ = pv_;
      }
    }

    // -------------------------------------------------------------------------
    // 6. Command Synthesis & C0-Continuous Over-Setpoint Safety Envelope
    // -------------------------------------------------------------------------
    float cmd = pHold_ + pTerm + integral_;
    if (!identified_ && heaterOnTime_ >= 0.0 && (clockS_ - heaterOnTime_) <= static_cast<double>(TAU_MAX_S) &&
        errTrue >= IDENT_MIN_ERR_C && !in.shadow && !doorDetected_) {
      cmd = capPct_;
    }

    // Two-stage C0-continuous over-setpoint attenuation:
    // Stage A (> 0.60*dQ): smoothly trims high-power climb/egg excess above 18% while leaving <= 18%
    //         cold-room steady-state hold untouched on normal single-step 0.1 C ripples.
    // Stage B (> max(0.05 C, 1.005*dQ)): smoothly scales ALL remaining power to 0.0% by ~SP + 0.155 C.
    const float overC = pv_ - sp_;
    const float highStart = std::max(0.03f, 0.60f * dQ);
    const float highSpan = std::max(0.045f, 0.45f * dQ);
    if (overC > highStart && cmd > 18.0f) {
      const float keepHigh = std::max(0.20f, 1.0f - (overC - highStart) / highSpan);
      cmd = 18.0f + (cmd - 18.0f) * keepHigh;
    }
    const float overStart = std::max(0.05f, 1.005f * dQ);
    const float overSpan = std::max(0.055f, 0.55f * dQ);
    if (overC > overStart && cmd > 0.0f) {
      const float atten = std::max(0.0f, 1.0f - (overC - overStart) / overSpan);
      cmd *= atten;
    }

    if (doorDetected_) {
      cmd = std::min(cmd, pHold_ + 6.0f);
      state_ = StDoor;
    } else if (in.shadow) {
      cmd = 0.0f;
      state_ = StFrozen;
    } else if (errTrue > 1.2f && filteredSlope_ >= 0.0f) {
      state_ = StClimb;
    } else {
      state_ = StTrack;
    }

    cmd = std::max(0.0f, std::min(capPct_, cmd));

    // -------------------------------------------------------------------------
    // 7. Update 1 Hz Transit Pipe & History Buffers
    // -------------------------------------------------------------------------
    secAcc_ += dt;
    liveS_ += dt;
    if (secAcc_ >= 1.0f) {
      secAcc_ -= 1.0f;
      pushPipe(static_cast<uint8_t>(std::round(in.shadow ? pHold_ : cmd)));
      pushHist(pv_, in.shadow ? 0.0f : cmd);
      if (in.shadow || doorDetected_ || in.ventFan) calmS_ = 0.0f;
      else calmS_ += 1.0f;
      updateMomentum();
      updateHoldBalance(in.permit && !in.shadow);
    }

    // -------------------------------------------------------------------------
    // 8. Heater Response Watchdog Flag (Telemetry)
    // -------------------------------------------------------------------------
    if (cmd > 40.0f && !in.shadow) {
      respHighPwrTimer_ += dt;
      if (std::isfinite(respPvStart_) && pv_ < respPvStart_) respPvStart_ = pv_;
      if (std::isfinite(respPvStart_) && (pv_ - respPvStart_) >= 0.20f) {
        respPvStart_ = pv_;
        respHighPwrTimer_ = 0.0f;
        respSuspect_ = false;
      } else if (respHighPwrTimer_ > 360.0f && std::isfinite(respPvStart_)) {
        respSuspect_ = true;
      }
    } else {
      respHighPwrTimer_ = 0.0f;
      respPvStart_ = pv_;
      respSuspect_ = false;
    }

    lastCmd_ = in.shadow ? 0.0f : cmd;
    out.powerPct = cmd;
    out.state = state_;
    out.powerLimited = (cmd >= capPct_);
    out.trustQ = doorDetected_ ? 6U : (identified_ ? 15U : 10U);
    return out;
  }

  // Telemetry & Persistence Readouts
  float predicted() const { return pvPredicted_; }
  uint8_t state() const { return state_; }
  uint8_t phase() const { return (state_ == StTrack || state_ == StDoor) ? 2U : (state_ == StClimb ? 1U : 0U); }
  bool responseSuspect() const { return respSuspect_; }
  bool identified() const { return identified_; }
  uint8_t batchesDone() const { return identified_ ? 2U : 0U; }
  float kAir() const { return kAir_; }
  float tauTransit() const { return tauTransit_; }
  uint16_t pipeLen() const { return pipeLen_; }
  float tdHat() const { return tauTransit_; }
  float tauHat() const { return pHold_; }
  float pHold() const { return pHold_; }
  float dNow() const { return pHold_; }
  float dLoss() const { return 0.0f; }
  float haPct() const { return 0.0f; }
  float sigmaPred() const { return 0.05f; }
  float integral() const { return integral_; }
  float slopeMks() const { return filteredSlope_ * 1000.0f; }
  bool doorDetected() const { return doorDetected_; }

  void importOven(float kAir, float tauTransit, uint16_t pipeLen) {
    if (std::isfinite(kAir)) kAir_ = std::max(KAIR_MIN, std::min(KAIR_MAX, kAir));
    if (std::isfinite(tauTransit)) tauTransit_ = std::max(TAU_MIN_S, std::min(TAU_MAX_S, tauTransit));
    pipeLen_ = std::max<uint16_t>(15U, std::min<uint16_t>(MAX_PIPE_LEN - 1U, pipeLen));
    ovenImported_ = true;
    pipeIdx_ = 0U;
    rebuildPipe();
  }

  void importHold(float pHold, bool measured) {
    if (std::isfinite(pHold)) pHold_ = std::max(0.0f, std::min(HOLD_MAX_PCT, pHold));
    identified_ = measured;
    holdImported_ = true;
    rebuildPipe();
  }

 private:
  void recomputePipeSum() {
    int32_t sum = 0;
    for (uint16_t i = 0U; i < pipeLen_; ++i) sum += pipe_[i];
    pipeSum_ = sum;
  }

  void rebuildPipe() {
    pipeIdx_ = 0U;
    for (uint16_t i = 0U; i < pipeLen_; ++i) {
      const uint16_t age = static_cast<uint16_t>(pipeLen_ - 1U - i);
      pipe_[i] = age < histCount_ ? uHist_[(histHead_ + HIST_LEN - 1U - age) % HIST_LEN] : static_cast<uint8_t>(pHold_);
    }
    recomputePipeSum();
  }

  void pushPipe(uint8_t value) {
    pipeSum_ += static_cast<int32_t>(value) - static_cast<int32_t>(pipe_[pipeIdx_]);
    pipe_[pipeIdx_] = value;
    pipeIdx_ = static_cast<uint16_t>((pipeIdx_ + 1U) % pipeLen_);
  }

  void pushHist(float pv, float cmd) {
    pvHist_[histHead_] = pv;
    uHist_[histHead_] = static_cast<uint8_t>(std::max(0.0f, std::min(100.0f, std::round(cmd))));
    histHead_ = static_cast<uint16_t>((histHead_ + 1U) % HIST_LEN);
    if (histCount_ < HIST_LEN) ++histCount_;
  }

  float pvAge(uint16_t age) const {
    return pvHist_[(histHead_ + HIST_LEN - 1U - age) % HIST_LEN];
  }

  float uAge(uint16_t age) const {
    return static_cast<float>(uHist_[(histHead_ + HIST_LEN - 1U - age) % HIST_LEN]);
  }

  float delayedPower(uint16_t delay, uint16_t n) const {
    if (n == 0U || histCount_ < static_cast<uint16_t>(delay + n + 1U)) return 0.0f;
    float sum = 0.0f;
    for (uint16_t i = 0U; i < n; ++i) sum += uAge(static_cast<uint16_t>(delay + i));
    return sum / static_cast<float>(n);
  }

  void updateHoldBalance(bool active) {
    if (!active || kAir_ <= 0.0f) return;
    const float spanS = std::min(LEARN_WINDOW_S, std::min(calmS_, liveS_));
    if (spanS < LEARN_MIN_S) return;
    const uint16_t w = static_cast<uint16_t>(spanS);
    const uint16_t delay = static_cast<uint16_t>(tauTransit_ + 0.5f);
    if (histCount_ < static_cast<uint16_t>(w + delay + 1U)) return;
    if (liveS_ < static_cast<float>(w + delay + 1U)) return;

    const float xm = 0.5f * static_cast<float>(w - 1U);
    const float ref = pvAge(0U);
    float sxy = 0.0f;
    for (uint16_t i = 0U; i < w; ++i) {
      sxy += (static_cast<float>(i) - xm) * (pvAge(static_cast<uint16_t>(w - 1U - i)) - ref);
    }
    const float sxx = static_cast<float>(w) * (static_cast<float>(w) * static_cast<float>(w) - 1.0f) / 12.0f;
    const float slope = sxy / sxx;

    float uSum = 0.0f, uMin = 100.0f, uMax = 0.0f;
    for (uint16_t i = 0U; i < w; ++i) {
      const float u = uAge(static_cast<uint16_t>(delay + i));
      uSum += u;
      uMin = std::min(uMin, u);
      uMax = std::max(uMax, u);
    }
    const float uMean = uSum / static_cast<float>(w);
    const bool kKnown = ovenImported_ || peakClimbSlope_ >= 0.0005f;
    const float kEff = (!kKnown && slope > 0.0f && pv_ >= sp_ - 0.15f) ? std::min(kAir_, 0.00018f) : kAir_;
    const float corr = slope / kEff;
    const float observed = std::max(0.0f, std::min(std::min(HOLD_MAX_PCT, uMean + LEARN_FALL_MAX_PCT), uMean - corr));

    if (observed >= pHold_) {
      // Upward adaptation: prevent step transients during climbs or at-SP rising overshoot from inflating pHold_
      if ((liveS_ < 1800.0f || pv_ >= sp_ - 1.6f || slope > 0.0005f) && (uMax - uMin) > 25.0f) return;
      if (!identified_ && !ovenImported_ && (uMax - uMin > 20.0f || liveS_ < 540.0f)) return;
      const float maxUnkSlope = (pv_ < sp_ - 0.65f) ? 0.0020f : 0.0005f;
      if (!kKnown && (slope > maxUnkSlope || corr < -14.0f)) return;
      if (pv_ >= sp_ - 0.5f * dQ_ && slope > 0.0004f) return;
    } else {
      // Downward relaxation (warming eggs / warm room): fast release near/above SP; guard unmeasured below-SP climbs
      if (!kKnown && pv_ < sp_ - 0.15f && (slope > 0.0f || uMax - uMin > 20.0f || liveS_ < 540.0f)) return;
      if (!identified_ && !ovenImported_ && slope < 0.0f && (uMax - uMin > 20.0f || liveS_ < 360.0f)) return;
      if (!kKnown && corr < -12.0f) return;
    }

    const float gap = observed - pHold_;
    const float step = gap >= 0.0f ? std::min(LEARN_STEP_UP_MAX, gap / LEARN_TAU_UP_S)
                                   : std::max(-LEARN_STEP_DOWN_MAX, gap / LEARN_TAU_DOWN_S);
    pHold_ = std::max(0.0f, std::min(HOLD_MAX_PCT, pHold_ + step));
  }

  void fadeWhileHeldOff(float pv, float sp, float dt) {
    if (shadowS_ <= 0.0f) {
      shadowRefPv_ = pv;
      shadowRefS_ = 0.0f;
      shadowFade_ = false;
    }
    shadowS_ += dt;
    if (shadowS_ - shadowRefS_ >= 180.0f) {
      shadowFade_ = pv >= sp - 0.5f * dQ_ && pv >= shadowRefPv_ - 1.01f * dQ_;
      shadowRefPv_ = pv;
      shadowRefS_ = shadowS_;
    }
    if (shadowFade_) {
      pHold_ = std::max(0.0f, pHold_ - pHold_ * (dt / 180.0f));
    }
  }

  void updateMomentum() {
    if (histCount_ < MOMENTUM_WINDOW_S || liveS_ < static_cast<float>(MOMENTUM_WINDOW_S)) {
      slopeShort_ = 0.0f;
      slopeMed_ = 0.0f;
      return;
    }
    const float xm = 0.5f * static_cast<float>(MOMENTUM_WINDOW_S - 1U);
    const float ref = pvAge(0U);
    float sxy = 0.0f;
    for (uint16_t i = 0U; i < MOMENTUM_WINDOW_S; ++i) {
      sxy += (static_cast<float>(i) - xm) * (pvAge(static_cast<uint16_t>(MOMENTUM_WINDOW_S - 1U - i)) - ref);
    }
    const float sxx = static_cast<float>(MOMENTUM_WINDOW_S) *
                      (static_cast<float>(MOMENTUM_WINDOW_S) * static_cast<float>(MOMENTUM_WINDOW_S) - 1.0f) / 12.0f;
    slopeShort_ = sxy / sxx;

    if (histCount_ >= 120U && liveS_ >= 120.0f && calmS_ >= 120.0f) {
      constexpr uint16_t MED_W = 120U;
      const float mxm = 0.5f * static_cast<float>(MED_W - 1U);
      float msxy = 0.0f;
      for (uint16_t i = 0U; i < MED_W; ++i) {
        msxy += (static_cast<float>(i) - mxm) * (pvAge(static_cast<uint16_t>(MED_W - 1U - i)) - ref);
      }
      constexpr float msxx = static_cast<float>(MED_W) * (static_cast<float>(MED_W) * static_cast<float>(MED_W) - 1.0f) / 12.0f;
      slopeMed_ = msxy / msxx;
    } else {
      slopeMed_ = 0.0f;
    }
  }

  void updateSlope(float pv, double nowS) {
    if (slopeHistCount_ < SLOPE_HIST_CAP) {
      slopePvHist_[slopeHistHead_] = pv;
      slopeTimeHist_[slopeHistHead_] = nowS;
      slopeHistHead_ = (slopeHistHead_ + 1U) % SLOPE_HIST_CAP;
      ++slopeHistCount_;
    } else {
      const uint8_t oldest = slopeHistHead_;
      const float dtSpan = static_cast<float>(nowS - slopeTimeHist_[oldest]);
      if (dtSpan > 2.0f) {
        const float rawSlope = (pv - slopePvHist_[oldest]) / dtSpan;
        const float alpha = std::min(1.0f, static_cast<float>(nowS - slopeTimeHist_[(oldest + SLOPE_HIST_CAP - 1U) % SLOPE_HIST_CAP]) / 3.0f);
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

  float kAir_ = 0.0004f;
  float tauTransit_ = 150.0f;
  uint16_t pipeLen_ = 202U;
  float pHold_ = HOLD_COLD_INIT_PCT;
  float integral_ = 0.0f;
  float pvPredicted_ = 30.0f;

  uint16_t pipeIdx_ = 0U;
  uint8_t pipe_[MAX_PIPE_LEN] = {};
  int32_t pipeSum_ = 0;

  float pvHist_[HIST_LEN] = {};
  uint8_t uHist_[HIST_LEN] = {};
  uint16_t histHead_ = 0U;
  uint16_t histCount_ = 0U;

  float slopeShort_ = 0.0f;
  float slopeMed_ = 0.0f;
  float dQ_ = 0.1f;
  float liveS_ = 0.0f;
  float calmS_ = 0.0f;
  float lastCmd_ = 0.0f;
  bool wasLive_ = false;
  bool startDone_ = false;
  bool ovenImported_ = false;
  bool holdImported_ = false;
  bool probing_ = false;
  float probeT_ = 0.0f;
  float probePv_ = NAN;
  float probeStart_ = NAN;
  uint8_t probeQuiet_ = 0U;

  float secAcc_ = 0.0f;
  double clockS_ = 0.0;
  double heaterOnTime_ = -1.0;
  float pvAtOn_ = NAN;
  bool identified_ = false;

  float filteredSlope_ = 0.0f;
  float peakClimbSlope_ = 0.0f;
  uint8_t slopeHistCount_ = 0U;
  uint8_t slopeHistHead_ = 0U;
  float slopePvHist_[SLOPE_HIST_CAP] = {};
  double slopeTimeHist_[SLOPE_HIST_CAP] = {};

  bool doorDetected_ = false;
  bool doorArmed_ = true;
  float doorTimer_ = 0.0f;
  float doorCalmS_ = 0.0f;
  float stuckTimer_ = 0.0f;
  float stuckAnchorPv_ = NAN;

  float shadowS_ = 0.0f;
  float shadowRefPv_ = NAN;
  float shadowRefS_ = 0.0f;
  bool shadowFade_ = false;

  bool respSuspect_ = false;
  float respHighPwrTimer_ = 0.0f;
  float respPvStart_ = NAN;
};

}  // namespace thermalv3
