#pragma once
// =========================================================================================
// Thermal V3.0.0 - Robust Industrial Incubation Thermal Controller
// Pure C++11 (no Arduino dependencies), header-only.
//
// Key principles:
// 1. Direct Physical Transit Measurement (Stopwatch identification of tau_transit) on a real start only.
// 2. Direct Physical Gain Measurement (Peak slope observed during a climb the heater really spent at its ceiling).
// 3. Energy-In-Flight Predictive Control:
//      PV_predicted = PV + k_air * sum(P_pipe - P_hold)
//    The model may only BRAKE near and far from SP (never add power the measured PV does not ask for); a measured momentum brake
//    (PV slope x transit time) backs it up where the model's picture of the load is wrong.
// 4. Asymmetric Anti-Windup Proportional-Integral tracking with slow base hold adaptation.
// 5. Active Disturbance Rejection:
//      - Door-Open Guard (prevents dangerous heat accumulation while doors are open; bounded in time).
// 6. Hold level learned from the measured energy balance, everywhere (not only next to SP):
//      hold = (power that reached the oven, delayed by the transit time) - (measured slope / heater gain)
//    The model above counts commanded power above the hold level as heat that is on its way. A tray of cold eggs
//    (or any other heat sink) takes most of that heat, so with a fixed 18 % hold the model promised +2 degC that never
//    came, throttled the heater to 60 % while the oven was 4 degC under SP, and kept doing so for as long as the eggs
//    were cold (the batch of 29/10: E115 cut the heater of a healthy oven). Measuring the balance keeps the model honest
//    for any load, with no number about the load given. Every step of that learning is rate-limited and bounded, and where
//    gain or delay have not been measured (a restart) it reads the balance only where it cannot depend on them.
// =========================================================================================
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace thermalv3 {

constexpr float KP_UP = 30.0f;      // Proportional gain below SP [% / degC]
constexpr float KP_DOWN = 90.0f;    // Proportional gain above SP (3x asymmetric)
constexpr float KI = 0.020f;        // Integral rate [% / (degC * s)]
constexpr float I_CLAMP = 15.0f;    // Hard anti-windup clamp [%]
// Near SP the controller decides from the MEASURED PV only (no energy-in-flight model): a PI whose gain follows the two measured
// numbers of the oven (heater gain and transport delay).  The model drives the climb only; between the two the weight fades linearly.
constexpr float MODEL_FAR_C = 1.0f;     // |error| at/above this: the energy-in-flight model decides (climb)
constexpr float MODEL_NEAR_C = 0.3f;    // |error| at/below this: measured PV only (hold)
constexpr float KP_HOLD_MIN = 4.0f;     // lowest hold-regime proportional gain [% / degC]
constexpr float HOLD_MAX_PCT = 85.0f;   // ceiling of the learned hold level [%]: cold eggs can take most of the heater to keep the air flat
constexpr float KAIR_MIN = 5.0e-5f;     // heater gain limits [degC/s per %]: a 20 kW heater on 1.8 MJ/K is 1.1e-4, on 0.3 MJ/K it is 6.7e-4
constexpr float KAIR_MAX = 7.5e-4f;
constexpr float DOOR_MAX_S = 300.0f;    // the door guard holds the heater back for at most this long per event
constexpr float DOOR_REARM_SLOPE = -0.006f;   // after a time-out the guard re-arms only once the fall has slowed below this [degC/s]
constexpr float LEARN_WINDOW_S = 240.0f;      // energy-balance window: the last 4 minutes of PV against the power that reached the oven (a 0.1 degC step is 0.0004 degC/s over it)
constexpr float LEARN_MIN_S = 90.0f;          // shortest window that is still trusted (right after a disturbance)
constexpr float LEARN_TAU_UP_S = 120.0f;      // the hold level follows a RISING balance (a sink appeared) with this time constant ...
constexpr float LEARN_TAU_DOWN_S = 40.0f;     // ... and a FALLING one (the sink is gone: a shut door, removed eggs) much faster: a hold level that is too high overshoots
constexpr float LEARN_STEP_UP_MAX = 0.5f;     // largest change of the hold level per second [%]
constexpr float LEARN_STEP_DOWN_MAX = 1.5f;
constexpr float LEARN_FALL_MAX_PCT = 30.0f;   // a falling PV may lift the hold level at most this far above the power that reached the oven
constexpr float LEARN_FLAT_CORR_PCT = 12.0f;  // without a measured heater gain the balance is read only where slope / gain is below this [%]
constexpr float LEARN_FLAT_U_RANGE = 20.0f;   // ... and where the power that reached the oven did not change by more than this [%]
constexpr float LEARN_FLAT_LIVE_S = 600.0f;   // ... and after the controller has been running this long (the response of a start is over)
constexpr float TAU_MIN_S = 90.0f;            // the transit time is never taken shorter than this: a stopwatch that ran while the energy of an earlier
constexpr float TAU_MAX_S = 220.0f;           // command was still on its way reads too short, and a delay that is too short makes every brake too weak
constexpr float IDENT_MIN_ERR_C = 2.0f;       // the transit time is timed only on a start at least this far under SP ...
constexpr float IDENT_MAX_SLOPE = 0.002f;     // ... from a PV that is not moving [degC/s]
constexpr uint16_t MOMENTUM_WINDOW_S = 40U;   // PV slope window of the momentum brake [s]
constexpr float MOMENTUM_NOISE_STEPS = 1.5f;  // a slope below this many sensor steps over that window is quantisation, not momentum
// 2.5.7: two rules against the overshoot of the restart after a protection cut the heater (the field log of 31/10: the High line at SV+0.2 over and over).
// The switches exist only so that tools/thermal-research/fleet-bench.cpp can compare with and without (-DV3X_F1=0 -DV3X_F4=0); the machine runs both.
#ifndef V3X_F1
#define V3X_F1 1     // the heater is held off by a protection and the reading does not fall: the hold level fades (as it does under the vent-line hold)
#endif
#ifndef V3X_F4
#define V3X_F4 1     // the controller drove the reading over SV by V3X_F4_OVER and a protection cut the heater: the hold level was too big, it keeps a share of itself
#endif
#ifndef V3X_F4_KEEP
#define V3X_F4_KEEP 0.6f
#endif
#ifndef V3X_F4_OVER
#define V3X_F4_OVER 0.15f
#endif
#ifndef V3X_HOLD0
#define V3X_HOLD0 18.0f   // the hold level of an oven nobody has measured (the bench compares other values)
#endif
constexpr uint16_t HIST_LEN = 640U;           // seconds of history: transit time (<=220 s) + learning window + margin

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

    // Priors of an oven nobody has measured (a controller that restarts next to SP never sees a start to measure). They lean to the fast,
    // high-gain, long-delay side of the ovens this controller meets, because that is the side on which wrong numbers make it swing:
    // gains and brakes sized for them are only slow on a slower oven, and the first climb replaces them with measurements anyway.
    kAir_ = 0.0004f;           // [degC/s per 100 % power]
    tauTransit_ = 150.0f;      // transport delay [seconds]
    pHold_ = V3X_HOLD0;        // nominal hold power [%]
    integral_ = 0.0f;
    pvPredicted_ = 30.0f;

    pipeLen_ = 202U;
    pipeIdx_ = 0U;
    for (uint16_t i = 0U; i < MAX_PIPE_LEN; ++i) pipe_[i] = static_cast<uint8_t>(pHold_);
    recomputePipeSum();

    histHead_ = 0U;
    histCount_ = 0U;
    slopeShort_ = 0.0f;
    wasLive_ = false;
    dQ_ = 0.1f;
    liveS_ = 0.0f;
    calmS_ = 0.0f;
    lastCmd_ = 0.0f;

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
    doorTimer_ = 0.0f;
    doorArmed_ = true;
    doorCalmS_ = 0.0f;
    stuckTimer_ = 0.0f;
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

    const bool pvValid = std::isfinite(in.pv) && in.pv > 0.0f && in.pv < 60.0f;
    const bool spValid = std::isfinite(in.sp) && in.sp > 20.0f && in.sp < 50.0f;
    const bool live = (in.permit || in.shadow) && pvValid && spValid;
    capPct_ = (std::isfinite(in.capPct) && in.capPct > 0.0f) ? std::min(100.0f, in.capPct) : 0.0f;

    if (!live || capPct_ <= 0.0f) {
      state_ = StFrozen;
      out.powerPct = 0.0f;
      out.state = state_;
      // The heater is not conducting: what was in flight drains out of the model and the history records the truth (zeros), so the
      // first command after the pause is not computed from the commands that came before it.
      // The heater is held off by a protection (High, the Web, a lost permit) while the reading is valid: it is a measurement. The controller was in charge
      // one cycle ago and the reading is over SV by 0.15 degC or more: its own hold level made that overshoot, and it keeps 60 % of itself (once per cut).
      // Then, as under the vent-line hold, every 3 minutes of a reading that stays at/above SV - 0.05 and does not fall more than one sensor step says the
      // oven loses little heat: the hold level fades (time constant 180 s). If the reading falls later the usual rules raise it again.
      if (pvValid && spValid && capPct_ > 0.0f && !in.permit && !in.shadow) {
#if V3X_F4
        if (wasLive_ && in.pv >= in.sp + V3X_F4_OVER) pHold_ *= V3X_F4_KEEP;
#endif
#if V3X_F1
        fadeWhileHeldOff(in.pv, in.sp, dt);
#endif
      }
      wasLive_ = false;
      liveS_ = 0.0f;
      calmS_ = 0.0f;
      lastCmd_ = 0.0f;
      if (!identified_) heaterOnTime_ = -1.0;       // a pause breaks the stopwatch of the start
      secAcc_ += dt;
      while (secAcc_ >= 1.0f) {
        secAcc_ -= 1.0f;
        pushPipe(0U);
        pushHist(std::isfinite(pv_) ? pv_ : 0.0f, 0.0f);
      }
      return out;
    }

    wasLive_ = true;
    pv_ = in.pv;
    sp_ = in.sp;

    // -------------------------------------------------------------------------
    // 1. Sliding-Window Slope Estimation (10s to 25s window)
    // -------------------------------------------------------------------------
    updateSlope(pv_, clockS_);

    // -------------------------------------------------------------------------
    // 2. Physical Transit Time & Gain Discovery
    // -------------------------------------------------------------------------
    if (!std::isfinite(respPvStart_)) respPvStart_ = pv_;

    // Transit time: a stopwatch from "the heater is put to work at (nearly) full power on an oven that is far under SP and not moving"
    // to the first 0.15 degC of rise. Only a real start is a clean measurement. A controller that restarts next to the set-point (an
    // update, a power blip) or in the middle of a fall has nothing to time: the first 0.15 degC it happens to see would be taken for the
    // delay of the oven and the energy balance below would be aligned on it.
    if (!identified_) {
      if (heaterOnTime_ < 0.0) {
        if (in.permit && !in.shadow && slopeHistCount_ >= SLOPE_HIST_CAP && std::fabs(filteredSlope_) <= IDENT_MAX_SLOPE &&
            (sp_ - pv_) >= IDENT_MIN_ERR_C && lastCmd_ >= 0.8f * capPct_) {
          heaterOnTime_ = clockS_;
          pvAtOn_ = pv_;
        }
      } else if (lastCmd_ < 0.4f * capPct_) {
        heaterOnTime_ = -1.0;                       // the heater was backed off: the stopwatch no longer times a full-power start
      } else if ((pv_ - pvAtOn_) >= 0.15f) {
        const float measuredDelay = static_cast<float>(clockS_ - heaterOnTime_);
        tauTransit_ = std::max(TAU_MIN_S, std::min(TAU_MAX_S, measuredDelay * 1.05f));
        // Total effective prediction horizon includes heater lag ~ 1.35x
        pipeLen_ = std::max<uint16_t>(15U, std::min<uint16_t>(MAX_PIPE_LEN - 1U, static_cast<uint16_t>(tauTransit_ * 1.35f)));
        rebuildPipe();
        identified_ = true;
      }
    }

    // Heater gain: the steepest 40 s least-squares slope seen while the heater really is at its ceiling (the 3 s slope of a 0.1 degC sensor
    // is a staircase, and its peaks are the stairs). "Really": the power that reached the oven during that slope, one transit time ago,
    // was at least 90 % of the ceiling, so the slope is the answer of the oven to that power and to nothing else. The hold level used
    // here is capped: a large learned hold (cold eggs) must not push the gain up, because a gain that is too big only makes the controller
    // cautious, one that is too small makes it rash.
    if (pv_ < sp_ - 1.0f && slopeShort_ > peakClimbSlope_ && in.permit) {
      const float uWin = delayedPower(static_cast<uint16_t>(tauTransit_ + 0.5f), MOMENTUM_WINDOW_S);
      if (uWin >= 0.9f * capPct_) {
        peakClimbSlope_ = slopeShort_;
        const float estK = peakClimbSlope_ / std::max(10.0f, uWin - std::min(pHold_, 40.0f));
        kAir_ = std::max(KAIR_MIN, std::min(KAIR_MAX, estK));
      }
    }

    // -------------------------------------------------------------------------
    // 3. Active Disturbance Rejection
    // -------------------------------------------------------------------------
    // Door Open Plunge Detection: temperature falling fast (< -0.7 C/min) below SP. The guard holds the heater back for the time the
    // door is really open; it lets go when the fall stops, and in any case after DOOR_MAX_S (a cold tray keeps pulling the air down long
    // after the door is shut, and a heater held at the hold level while the oven needs it only deepens the dip). After a time-out it
    // is armed again only once the fall has slowed, so one long fall is not clamped twice.
    if (filteredSlope_ > DOOR_REARM_SLOPE) doorCalmS_ += dt; else doorCalmS_ = 0.0f;
    if (!doorArmed_ && !doorDetected_ && doorCalmS_ >= 20.0f) doorArmed_ = true;
    if (doorArmed_ && filteredSlope_ < -0.012f && (sp_ - pv_) > 0.40f) {
      doorDetected_ = true;
      doorArmed_ = false;
      doorTimer_ = 0.0f;
    }
    if (doorDetected_) {
      doorTimer_ += dt;
      if ((filteredSlope_ > -0.003f && doorTimer_ > 15.0f) || doorTimer_ > DOOR_MAX_S) {
        doorDetected_ = false;
      }
    }

    // -------------------------------------------------------------------------
    // 4. Energy-In-Flight Predictive Calculation (drives the climb; fades out near SP)
    // -------------------------------------------------------------------------
    // Every threshold below that compares PV with SP is a multiple of the sensor step dQ (0.1 or 0.01 degC): the same code is flat on a
    // coarse sensor and tight on a fine one, and one quantum of a coarse sensor never counts as a real error.
    const float dQ = (std::isfinite(in.dQ) && in.dQ >= 0.005f && in.dQ <= 0.2f) ? in.dQ : 0.1f;
    dQ_ = dQ;
    auto deadZone = [dQ](float e) { return e > 0.5f * dQ ? e - 0.5f * dQ : (e < -0.5f * dQ ? e + 0.5f * dQ : 0.0f); };

    const float excessEnergy = static_cast<float>(pipeSum_) - static_cast<float>(pipeLen_) * pHold_;
    pvPredicted_ = std::max(pv_ - 0.20f, pv_ + kAir_ * excessEnergy);

    const float errPred = sp_ - pvPredicted_;
    const float errTrue = sp_ - pv_;

    // -------------------------------------------------------------------------
    // 5. Control Law: P on a blend of predicted (far) and measured (near) error + Conditional Anti-Windup I
    // -------------------------------------------------------------------------
    const float wModel = std::max(0.0f, std::min(1.0f, (std::fabs(errTrue) - MODEL_NEAR_C) / (MODEL_FAR_C - MODEL_NEAR_C)));
    const float kpHold = std::max(KP_HOLD_MIN, std::min(KP_UP, 1.0f / (2.0f * kAir_ * static_cast<float>(pipeLen_))));
    // The far-field gain is sized to the oven as well: 1 degC of error asks for the power that would move the air by 1 degC within one
    // horizon (gain x horizon = kAir x pipeLen). 30 %/degC is right for a slow oven (kAir x pipeLen < 0.033); an oven that answers fast
    // after a long delay (small air mass, 20 kW) swung by +-1 degC under it.
    const float kpFar = std::max(kpHold, std::min(KP_UP, 1.0f / (kAir_ * static_cast<float>(pipeLen_))));
    const float kp = wModel * kpFar + (1.0f - wModel) * kpHold;
    // Near SP the model may only BRAKE (it sees power already on its way that will carry PV over SP); it may not ADD power for an
    // energy "deficit" it infers from the commanded history (a held heater, a changed hold level): the measured PV decides that.
    const float errMeas = deadZone(errTrue);
    // Momentum brake: whatever the model believes, PV keeps rising at about the present slope for one transit time after the heater is
    // cut. It is measured, so it is right when the model is not (the load that held the hold level up has just gone: a door shut, a tray
    // taken out) and it is the same size as the model's lift when the model is right.
    const float errMomentum = deadZone(errTrue - std::max(0.0f, slopeShort_ - MOMENTUM_NOISE_STEPS * dQ_ / static_cast<float>(MOMENTUM_WINDOW_S)) * tauTransit_);
    const float errModel = std::min(deadZone(errPred), errMomentum);
    const float errCtl = wModel * errModel + (1.0f - wModel) * std::min(errMeas, errModel);
    const float pTerm = errCtl >= 0.0f ? kp * errCtl : KP_DOWN * errCtl;   // strong damping above SP against overshoot

    // The vent line holds the heater at 0 %: the controller is not in charge, so nothing is learned from the error (no integral, no hold,
    // no stuck rule). But the held heater is also a measurement: every 3 minutes of hold, if PV stayed at/above SP - 0.05 and did not fall
    // more than one 0.1 degC step, the oven loses little heat and the stored hold level is too big (eggs warm): it then fades (time constant
    // 180 s). If PV falls later the normal rules raise the hold again.
    if (in.shadow) {
      if (shadowS_ <= 0.0f) { shadowRefPv_ = pv_; shadowRefS_ = 0.0f; shadowFade_ = false; }
      shadowS_ += dt;
      if (shadowS_ - shadowRefS_ >= 180.0f) {
        shadowFade_ = pv_ >= sp_ - 0.5f * dQ && pv_ >= shadowRefPv_ - 1.01f * dQ;
        shadowRefPv_ = pv_;
        shadowRefS_ = shadowS_;
      }
      if (shadowFade_) pHold_ = std::max(0.0f, pHold_ - pHold_ * (dt / 180.0f));
      stuckTimer_ = 0.0f;
    } else {
      shadowS_ = 0.0f;
      shadowFade_ = false;
    }

    // Integral term on True Error (active within +-0.75 degC)
    if (!in.shadow && !doorDetected_ && std::fabs(errTrue) <= 0.75f) {
      integral_ += std::min(KI, kpHold / (4.0f * static_cast<float>(pipeLen_))) * deadZone(errTrue) * dt;   // Ti >= 4 x the measured delay (SIMC), never faster than before
      integral_ = std::max(-I_CLAMP, std::min(I_CLAMP, integral_));

      // Slow absorption of persistent integral into pHold (time constant ~300s)
      const float shift = integral_ * (dt / 300.0f);
      pHold_ = std::max(0.0f, std::min(HOLD_MAX_PCT, pHold_ + shift));
      integral_ -= shift;
    } else if (!in.shadow && !doorDetected_) {
      integral_ *= std::max(0.0f, 1.0f - 0.05f * dt);
    }

    // Anti-Stuck protection: if temperature rests flat one sensor step or more (but at least 0.08 degC) below SP for > 120 s. It used to wait for
    // three steps; a PV that stood 0.2 degC under SP was then too far for the proportional term and not far enough for this rule (the log of 30/10:
    // 37.3 degC for 20 minutes).
    if (!in.shadow && !doorDetected_ && errTrue >= std::max(0.08f, 0.95f * dQ) && std::fabs(filteredSlope_) < 0.0015f) {
      stuckTimer_ += dt;
      if (stuckTimer_ > 120.0f) {
        pHold_ = std::min(HOLD_MAX_PCT, pHold_ + 0.025f * dt);
      }
    } else if (!in.shadow) {
      stuckTimer_ = 0.0f;
    }

    // Raw commanded power
    float cmd = pHold_ + pTerm + integral_;

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
    liveS_ += dt;
    if (secAcc_ >= 1.0f) {
      secAcc_ -= 1.0f;
      // A heater held off by the vent line is not a command the model should count as missing energy: it records the hold level.
      pushPipe(static_cast<uint8_t>(std::round(in.shadow ? pHold_ : cmd)));
      // The history keeps what was physically sent (a held heater sent nothing). Calm = no door, no vent-line hold, no fan: the
      // balance below is only read over a calm stretch.
      pushHist(pv_, in.shadow ? 0.0f : cmd);
      if (in.shadow || doorDetected_ || in.ventFan) calmS_ = 0.0f; else calmS_ += 1.0f;
      updateMomentum();
      learnBalance(in.permit);
    }

    // -------------------------------------------------------------------------
    // 7. Safety Watchdog
    // -------------------------------------------------------------------------
    if (cmd > 40.0f) {
      respHighPwrTimer_ += dt;
      // The reference follows the lowest PV (a dip must not leave a stale high reference behind) and a rise of 0.2 degC starts a new
      // window, the same way the E115 evidence works (heater_response_watch.h).
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

  // Telemetry readouts for machine supervision and web monitors
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

  // What an earlier run of THIS oven learned (see thermal_learn.h, which keeps it across a restart).
  // Heater gain and transit time are properties of the oven: they come back as the starting numbers of the next first climb, which measures
  // them again (identified_ stays false). Nothing here is trusted more than the measurement that follows it.
  void importOven(float kAir, float tauTransit, uint16_t pipeLen) {
    if (std::isfinite(kAir)) kAir_ = std::max(KAIR_MIN, std::min(KAIR_MAX, kAir));
    if (std::isfinite(tauTransit)) tauTransit_ = std::max(TAU_MIN_S, std::min(TAU_MAX_S, tauTransit));
    pipeLen_ = std::max<uint16_t>(15U, std::min<uint16_t>(MAX_PIPE_LEN - 1U, pipeLen));
    pipeIdx_ = 0U;
    rebuildPipe();
  }
  // The hold level of the batch that is being resumed after a restart (the load did not change: same eggs, same room) and the fact that the
  // numbers above were measured on this oven. The learner keeps working: it follows the load from here, fast down and slow up as always.
  void importHold(float pHold, bool measured) {
    if (std::isfinite(pHold)) pHold_ = std::max(0.0f, std::min(HOLD_MAX_PCT, pHold));
    identified_ = measured;
    rebuildPipe();
  }

 private:
  void recomputePipeSum() {
    int32_t sum = 0;
    for (uint16_t i = 0U; i < pipeLen_; ++i) sum += pipe_[i];
    pipeSum_ = sum;
  }
  // A new horizon: the ring must hold the last pipeLen_ commands that really went out, oldest at the write position. Entries the history
  // does not reach back to are the hold level (nothing was in flight before the controller started). The write position goes back to 0
  // with the oldest command: it may lie beyond a shorter ring, and a push there would corrupt the running sum.
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
  float pvAge(uint16_t age) const { return pvHist_[(histHead_ + HIST_LEN - 1U - age) % HIST_LEN]; }
  float uAge(uint16_t age) const { return static_cast<float>(uHist_[(histHead_ + HIST_LEN - 1U - age) % HIST_LEN]); }

  // Mean power that reached the oven during the n seconds that ended `delay` seconds ago (history: 1 Hz, newest last). Returns 0 while
  // the history is shorter than that.
  float delayedPower(uint16_t delay, uint16_t n) const {
    if (n == 0U || histCount_ < static_cast<uint16_t>(delay + n + 1U)) return 0.0f;
    float sum = 0.0f;
    for (uint16_t i = 0U; i < n; ++i) sum += uAge(static_cast<uint16_t>(delay + i));
    return sum / static_cast<float>(n);
  }

  // Energy balance, once per second. The power that reached the oven over the last w seconds is the command of one transit time earlier;
  // the measured slope of PV over the same w seconds says how much of it moved the temperature (slope / heater gain). The rest is what
  // the oven loses or soaks up: the power that would hold PV flat right now. Heater gain and transit time are the ones measured on the
  // first climb; nothing about the load is assumed, so cold eggs, a door, a different cart all show up as a different hold level.
  //
  // Where gain or delay have not been measured (the controller restarted next to SP) the balance is read only where it cannot depend
  // on them: a PV that is nearly flat and a power that did not change. A wrong gain can never turn the balance into a runaway.
  void learnBalance(bool permit) {
    if (!permit || kAir_ <= 0.0f) return;
    const float spanS = std::min(LEARN_WINDOW_S, std::min(calmS_, liveS_));
    if (spanS < LEARN_MIN_S) return;
    const uint16_t w = static_cast<uint16_t>(spanS);
    const uint16_t delay = static_cast<uint16_t>(tauTransit_ + 0.5f);
    if (histCount_ < static_cast<uint16_t>(w + delay + 1U)) return;     // the history does not reach that far back yet
    // The power that reached the oven during the window was commanded one transit time earlier, and that stretch must belong to this run as
    // well: across a pause (a controller that has been idle since power-up, a batch that starts later) the history holds the idle, the power
    // steps from nothing to full inside the window, and the answer of the oven to a step is spread over its whole delay: the balance would read
    // a hold level of about nothing right at a start.
    if (liveS_ < static_cast<float>(w + delay + 1U)) return;
    // Least-squares slope of the last w seconds of PV (x = seconds, oldest first).
    const float xm = 0.5f * static_cast<float>(w - 1U);
    const float ref = pvAge(0U);                                        // differences to the newest value: a 37 degC offset would cost float precision
    float sxy = 0.0f;
    for (uint16_t i = 0U; i < w; ++i) sxy += (static_cast<float>(i) - xm) * (pvAge(static_cast<uint16_t>(w - 1U - i)) - ref);
    const float sxx = static_cast<float>(w) * (static_cast<float>(w) * static_cast<float>(w) - 1.0f) / 12.0f;
    const float slope = sxy / sxx;
    // Mean power that was reaching the oven during that stretch (and how much it moved).
    float uSum = 0.0f, uMin = 100.0f, uMax = 0.0f;
    for (uint16_t i = 0U; i < w; ++i) {
      const float u = uAge(static_cast<uint16_t>(delay + i));
      uSum += u;
      uMin = std::min(uMin, u);
      uMax = std::max(uMax, u);
    }
    const float uMean = uSum / static_cast<float>(w);
    const float corr = slope / kAir_;                                   // power that went into the temperature rather than into the losses [%]
    if (!identified_ && (uMax - uMin > LEARN_FLAT_U_RANGE || liveS_ < LEARN_FLAT_LIVE_S)) return;
    if (peakClimbSlope_ < 0.0005f && std::fabs(corr) > LEARN_FLAT_CORR_PCT) return;
    const float observed = std::max(0.0f, std::min(std::min(HOLD_MAX_PCT, uMean + LEARN_FALL_MAX_PCT), uMean - corr));
    // Asymmetric: a hold level that is too LOW only makes the oven slow, one that is too HIGH makes it overshoot.
    const float gap = observed - pHold_;
    const float step = gap >= 0.0f ? std::min(LEARN_STEP_UP_MAX, gap / LEARN_TAU_UP_S)
                                   : std::max(-LEARN_STEP_DOWN_MAX, gap / LEARN_TAU_DOWN_S);
    pHold_ = std::max(0.0f, std::min(HOLD_MAX_PCT, pHold_ + step));
  }

  // Slope of the last MOMENTUM_WINDOW_S seconds of PV, once per second (least squares on the 1 Hz history): the momentum brake reads it.
  void fadeWhileHeldOff(float pv, float sp, float dt) {
    if (shadowS_ <= 0.0f) { shadowRefPv_ = pv; shadowRefS_ = 0.0f; shadowFade_ = false; }
    shadowS_ += dt;
    if (shadowS_ - shadowRefS_ >= 180.0f) {
      shadowFade_ = pv >= sp - 0.5f * dQ_ && pv >= shadowRefPv_ - 1.01f * dQ_;
      shadowRefPv_ = pv;
      shadowRefS_ = shadowS_;
    }
    if (shadowFade_) pHold_ = std::max(0.0f, pHold_ - pHold_ * (dt / 180.0f));
  }
  void updateMomentum() {
    if (histCount_ < MOMENTUM_WINDOW_S || liveS_ < static_cast<float>(MOMENTUM_WINDOW_S)) { slopeShort_ = 0.0f; return; }
    const float xm = 0.5f * static_cast<float>(MOMENTUM_WINDOW_S - 1U);
    const float ref = pvAge(0U);
    float sxy = 0.0f;
    for (uint16_t i = 0U; i < MOMENTUM_WINDOW_S; ++i) sxy += (static_cast<float>(i) - xm) * (pvAge(static_cast<uint16_t>(MOMENTUM_WINDOW_S - 1U - i)) - ref);
    const float sxx = static_cast<float>(MOMENTUM_WINDOW_S) * (static_cast<float>(MOMENTUM_WINDOW_S) * static_cast<float>(MOMENTUM_WINDOW_S) - 1.0f) / 12.0f;
    slopeShort_ = sxy / sxx;
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
  float pHold_ = 18.0f;
  float integral_ = 0.0f;
  float pvPredicted_ = 30.0f;

  uint16_t pipeLen_ = 202U;
  uint16_t pipeIdx_ = 0U;
  uint8_t pipe_[MAX_PIPE_LEN] = {};
  int32_t pipeSum_ = 0;          // sum of the pipeLen_ entries in use

  float pvHist_[HIST_LEN] = {};  // 1 Hz: the PV the controller saw
  uint8_t uHist_[HIST_LEN] = {}; // 1 Hz: the command that was physically sent [%]
  uint16_t histHead_ = 0U;
  uint16_t histCount_ = 0U;
  bool wasLive_ = false;         // the last cycle ran the control law (a permit lost now, with the reading over SV, is the controller's own overshoot)
  float slopeShort_ = 0.0f;      // [degC/s] least-squares slope of the last MOMENTUM_WINDOW_S seconds
  float dQ_ = 0.1f;              // sensor step of the last call [degC]
  float liveS_ = 0.0f;           // seconds the controller has been running without a pause
  float calmS_ = 0.0f;           // seconds without a door, a vent-line hold or a vent fan
  float lastCmd_ = 0.0f;         // the command of the previous call [%] (what the oven was given)

  float secAcc_ = 0.0f;
  // The time base is a double: a float that accumulates 0.1 s steps runs 25 % fast after 6 days and stops after about 20, and every slope
  // (door detection included) is a difference of two of its readings.
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
  float shadowS_ = 0.0f;         // seconds the vent line has been holding the heater at 0 %
  float shadowRefPv_ = NAN;      // PV at the last 3-minute checkpoint of that hold
  float shadowRefS_ = 0.0f;
  bool shadowFade_ = false;      // the hold level is fading (heater held off, PV did not fall)

  bool respSuspect_ = false;
  float respHighPwrTimer_ = 0.0f;
  float respPvStart_ = NAN;
};

}  // namespace thermalv3
