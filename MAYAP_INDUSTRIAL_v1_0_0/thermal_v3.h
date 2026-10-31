#pragma once
// =========================================================================================
// Thermal V3 (firmware 2.5.0) - the simple controller
// Pure C++11 (no Arduino dependencies), header-only.
//
// One state carries the whole picture of the oven: pHold, the power that holds the temperature flat. Everything else is a rule on the SIGN of
// the error: too low -> more, too high -> less. There is one writer of pHold (the slow integral) and two rules that nudge it on a measured
// stand-still (below SP and flat: up; above SP and not falling: down). There is no learner, no second model and no mode switch that fades
// between two laws.
//
//  1. Energy in flight. The commands of the last transit time are kept (the "pipe"). With
//         PV_predicted = PV + k_air * sum(P_pipe - P_hold)
//     the proportional term acts on the temperature the oven WILL show when the power on its way has arrived. That is what lets a high gain
//     (30 %/degC up, 90 %/degC down) brake early on a climb without swinging, whatever the delay. The prediction is never lower than PV - 0.2.
//  2. Two numbers measured on a real start: the transit time (a stopwatch from full power to the first 0.15 degC of rise) and the heater gain
//     (the steepest 40 s slope while the heater really is at its ceiling). A controller that restarts next to SP times nothing and keeps its priors.
//  3. The integral works in a band of +-1 degC and is absorbed into pHold only within 0.75 degC of SP: what it stored while far from SP is a lift for
//     the climb (a hold level that is wrong, cold eggs that soak up heat), not a hold level, and it is dropped the moment PV is over SP.
//  4. Stand-still rules, evaluated over a window longer than the transit time (a correction judged before it has arrived makes a limit cycle):
//       - below SP by a sensor step and flat: pHold creeps up, and when PV stands well under SP it follows the mean of the pipe up (the heat the
//         model counted as on its way did not come: a tray of cold eggs took it);
//       - at/over SP + one sensor step and not falling (the exhaust fans of this machine take little heat away, they cannot be counted on):
//         pHold is trimmed down. The harder cut well above SP is the job of the safety margin of the machine (the vent line holds the heater).
//  5. The door guard holds the heater back for the time the door is really open (bounded: a cold tray keeps pulling the air down long after).
// =========================================================================================
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace thermalv3 {

constexpr float KP_UP = 30.0f;          // Proportional gain below the predicted SP [% / degC]
constexpr float KP_DOWN = 90.0f;        // Proportional gain above it (3x asymmetric: an overshoot costs more than a slow climb)
constexpr float KI = 0.020f;            // Integral rate [% / (degC * s)]
constexpr float I_CLAMP = 15.0f;        // Hard anti-windup clamp of the integral [%]
constexpr float I_ABSORB_C = 0.75f;     // What the integral stored while far from SP is a lift for the climb, not a hold level: absorbed only this close
constexpr float I_BAND_C = 1.0f;        // The integral works while |error| is within this; it is absorbed into pHold only within I_ABSORB_C
constexpr float HOLD_MIN_PCT = 0.0f;    // A late batch can hold the temperature by itself: the hold level may go to nothing
constexpr float HOLD_MAX_PCT = 85.0f;   // Cold eggs can take most of the heater to keep the air flat
constexpr float KAIR_MIN = 5.0e-5f;     // Heater gain limits [degC/s per %]
constexpr float KAIR_MAX = 7.5e-4f;
constexpr float TAU_MIN_S = 90.0f;      // Transit time limits: a stopwatch that ran while earlier energy was still on its way reads too short
constexpr float TAU_MAX_S = 220.0f;
constexpr float IDENT_MIN_ERR_C = 2.0f;      // The transit time is timed only on a start at least this far under SP ...
constexpr float IDENT_MAX_SLOPE = 0.002f;    // ... from a PV that is not moving [degC/s]
constexpr uint16_t SLOPE_WINDOW_S = 40U;     // Window of the heater-gain slope [s]
constexpr float DOOR_MAX_S = 300.0f;         // The door guard holds the heater back for at most this long per event
constexpr float DOOR_REARM_SLOPE = -0.006f;  // After a time-out the guard re-arms only once the fall has slowed below this [degC/s]
constexpr float STUCK_FLAT_SLOPE = 0.0015f;  // "Flat": slower than this [degC/s]
constexpr float STUCK_UP_RATE = 0.025f;      // pHold creep when stuck under SP [% / s]
constexpr float FOLLOW_RATE = 0.30f;         // ... and the fastest it follows the mean of the pipe up [% / s]
constexpr float FOLLOW_TAU_S = 30.0f;        // ... with this time constant
constexpr float FOLLOW_MIN_STEPS = 5.0f;     // ... and only when PV is at least this many sensor steps under SP (a small gap is trimmed by the creep)
constexpr float OVER_TRIM_RATE = 0.025f;     // pHold trim when stuck over SP [% / s]
constexpr float OVER_NOT_FALLING = -0.0003f; // Over SP and slower than this fall [degC/s] counts as "not falling"
constexpr float OVER_ARM_S = 120.0f;         // Over SP and not falling this long before the first trim
constexpr uint16_t HIST_LEN = 320U;          // Seconds of command history: transit time (<= 220 s) + the 40 s heater-gain window + margin

enum State : uint8_t {
  StIdle = 0,    // Standby / Not permitted to heat
  StClimb = 1,   // Phase 1: High-power climb with automatic parameter discovery
  StTrack = 2,   // Phase 2: Steady-state tracking
  StDoor = 3,    // Disturbance: Door open detected (power clamped to prevent surge)
  StFrozen = 4   // Safety lock: Sensor missing, permit lost, or the vent line holds the heater
};

struct Input {
  float pv = NAN;                // Filtered process temperature [degC]
  float sp = 37.5f;              // Target setpoint [degC]
  float dQ = 0.1f;               // Sensor resolution step (0.1 or 0.01) [degC]
  float capPct = 100.0f;         // Configured maximum heater ceiling [%]
  float dtS = 0.1f;              // Step interval [seconds]
  bool permit = false;           // Production safety permit (all protections green)
  bool shadow = false;           // The vent line holds the heater at 0 % (keep the state alive)
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
    // high-gain, long-delay side: gains and brakes sized for them are only slow on a slower oven, and the first climb replaces them.
    kAir_ = 0.0004f;           // [degC/s per 100 % power]
    tauTransit_ = 150.0f;      // transport delay [seconds]
    pHold_ = 18.0f;            // nominal hold power [%]
    integral_ = 0.0f;
    pvPredicted_ = 30.0f;

    pipeLen_ = 202U;
    pipeIdx_ = 0U;
    for (uint16_t i = 0U; i < MAX_PIPE_LEN; ++i) pipe_[i] = static_cast<uint8_t>(pHold_);
    recomputePipeSum();

    histHead_ = 0U;
    histCount_ = 0U;
    dQ_ = 0.1f;
    lastCmd_ = 0.0f;

    secAcc_ = 0.0f;
    clockS_ = 0.0;
    heaterOnTime_ = -1.0;
    pvAtOn_ = NAN;
    identified_ = false;

    filteredSlope_ = 0.0f;
    slopeShort_ = 0.0f;
    peakClimbSlope_ = 0.0f;
    slopeHistCount_ = 0U;
    slopeHistHead_ = 0U;
    for (uint8_t i = 0U; i < SLOPE_HIST_CAP; ++i) {
      slopePvHist_[i] = 0.0f;
      slopeTimeHist_[i] = 0.0;
    }
    for (uint16_t i = 0U; i < HIST_LEN; ++i) {
      pvHist_[i] = 0.0f;
      uHist_[i] = 0U;
    }

    doorDetected_ = false;
    doorTimer_ = 0.0f;
    doorArmed_ = true;
    doorCalmS_ = 0.0f;
    stuckTimer_ = 0.0f;
    overTimer_ = 0.0f;
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
      lastCmd_ = 0.0f;
      overTimer_ = 0.0f;
      if (!identified_) heaterOnTime_ = -1.0;       // a pause breaks the stopwatch of the start
      secAcc_ += dt;
      while (secAcc_ >= 1.0f) {
        secAcc_ -= 1.0f;
        pushPipe(0U);
        pushHist(std::isfinite(pv_) ? pv_ : 0.0f, 0.0f);
      }
      return out;
    }

    pv_ = in.pv;
    sp_ = in.sp;
    const float dQ = (std::isfinite(in.dQ) && in.dQ >= 0.005f && in.dQ <= 0.2f) ? in.dQ : 0.1f;
    dQ_ = dQ;

    // -------------------------------------------------------------------------
    // 1. Sliding-window slope of PV
    // -------------------------------------------------------------------------
    updateSlope(pv_, clockS_);
    if (!std::isfinite(respPvStart_)) respPvStart_ = pv_;

    // -------------------------------------------------------------------------
    // 2. Transit time and heater gain, measured on a real start only
    // -------------------------------------------------------------------------
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
        // Total effective prediction horizon includes the heater lag: about 1.35x
        pipeLen_ = std::max<uint16_t>(15U, std::min<uint16_t>(MAX_PIPE_LEN - 1U, static_cast<uint16_t>(tauTransit_ * 1.35f)));
        rebuildPipe();
        identified_ = true;
      }
    }
    // Heater gain: the steepest 40 s slope seen while the heater really is at its ceiling (the power that reached the oven during that slope,
    // one transit time ago, was at least 90 % of the ceiling). The hold level used here is capped: a gain that is too big only makes the
    // controller cautious, one that is too small makes it rash.
    if (pv_ < sp_ - 1.0f && slopeShort_ > peakClimbSlope_ && in.permit) {
      const float uWin = delayedPower(static_cast<uint16_t>(tauTransit_ + 0.5f), SLOPE_WINDOW_S);
      if (uWin >= 0.9f * capPct_) {
        peakClimbSlope_ = slopeShort_;
        const float estK = peakClimbSlope_ / std::max(10.0f, uWin - std::min(pHold_, 40.0f));
        kAir_ = std::max(KAIR_MIN, std::min(KAIR_MAX, estK));
      }
    }

    // -------------------------------------------------------------------------
    // 3. Door guard: temperature falling fast below SP. It holds the heater back for the time the door is really open and lets go when the
    //    fall stops, and in any case after DOOR_MAX_S. After a time-out it re-arms only once the fall has slowed.
    // -------------------------------------------------------------------------
    if (filteredSlope_ > DOOR_REARM_SLOPE) doorCalmS_ += dt; else doorCalmS_ = 0.0f;
    if (!doorArmed_ && !doorDetected_ && doorCalmS_ >= 20.0f) doorArmed_ = true;
    if (doorArmed_ && filteredSlope_ < -0.012f && (sp_ - pv_) > 0.40f) {
      doorDetected_ = true;
      doorArmed_ = false;
      doorTimer_ = 0.0f;
    }
    if (doorDetected_) {
      doorTimer_ += dt;
      if ((filteredSlope_ > -0.003f && doorTimer_ > 15.0f) || doorTimer_ > DOOR_MAX_S) doorDetected_ = false;
    }

    // -------------------------------------------------------------------------
    // 4. Energy in flight and the proportional term
    // -------------------------------------------------------------------------
    const float excessEnergy = static_cast<float>(pipeSum_) - static_cast<float>(pipeLen_) * pHold_;
    pvPredicted_ = std::max(pv_ - 0.20f, pv_ + kAir_ * excessEnergy);
    const float errPred = sp_ - pvPredicted_;
    const float errTrue = sp_ - pv_;
    const float pTerm = errPred >= 0.0f ? KP_UP * errPred : KP_DOWN * errPred;   // strong damping above SP against overshoot

    // -------------------------------------------------------------------------
    // 5. Integral on the true error, absorbed slowly into pHold (the one writer of the hold level)
    // -------------------------------------------------------------------------
    if (!in.shadow && !doorDetected_ && std::fabs(errTrue) <= I_BAND_C) {
      integral_ += KI * errTrue * dt;
      if (errTrue < 0.0f && integral_ > 0.0f) integral_ = 0.0f;       // over SP: what was stored for the climb is not needed any more
      integral_ = std::max(-I_CLAMP, std::min(I_CLAMP, integral_));
      if (std::fabs(errTrue) <= I_ABSORB_C) {
        const float shift = integral_ * (dt / 300.0f);
        pHold_ = std::max(HOLD_MIN_PCT, std::min(HOLD_MAX_PCT, pHold_ + shift));
        integral_ -= shift;
      }
    } else if (!in.shadow && !doorDetected_) {
      integral_ *= std::max(0.0f, 1.0f - 0.05f * dt);
    }

    // -------------------------------------------------------------------------
    // 6. Stand-still rules (judged over a window longer than the transit time)
    // -------------------------------------------------------------------------
    const float stepC = std::max(0.08f, 0.95f * dQ);           // one real sensor step
    const float judgeS = std::max(120.0f, 1.2f * tauTransit_);
    // Under SP and flat: the hold level is too low. The model counted the power of the last transit time above pHold as heat on its way; PV
    // stood still, so that heat did not come (a tray of cold eggs took it, a door let it out) and what was sent is, for this oven right now,
    // no more than the hold level. pHold follows the mean of the pipe up, at a bounded rate, instead of creeping a fraction of a percent.
    if (!in.shadow && !doorDetected_ && errTrue >= stepC && std::fabs(filteredSlope_) < STUCK_FLAT_SLOPE) {
      stuckTimer_ += dt;
      if (stuckTimer_ > judgeS) {
        const float pipeMean = static_cast<float>(pipeSum_) / static_cast<float>(pipeLen_);
        const float follow = errTrue >= FOLLOW_MIN_STEPS * stepC ? std::min(FOLLOW_RATE, (pipeMean - pHold_) / FOLLOW_TAU_S) : 0.0f;
        pHold_ = std::min(HOLD_MAX_PCT, pHold_ + std::max(STUCK_UP_RATE, follow) * dt);
      }
    } else {
      stuckTimer_ = 0.0f;
    }
    // Over SP and not falling: the hold level is too high; trim it. The exhaust fans of this machine take little heat away, so PV does not come
    // down by itself: the one lever is the heater. A harder cut is the job of the safety margin above SP (the vent line holds the heater
    // at 0 % well past SV), which acts on its own and is bounded by its hysteresis.
    if (!in.shadow && !doorDetected_ && errTrue <= -stepC && filteredSlope_ >= OVER_NOT_FALLING) {
      overTimer_ += dt;
      if (overTimer_ > OVER_ARM_S) {
        pHold_ = std::max(HOLD_MIN_PCT, pHold_ - OVER_TRIM_RATE * dt);
        if (integral_ > 0.0f) integral_ = 0.0f;
      }
    } else if (errTrue > -stepC || filteredSlope_ < OVER_NOT_FALLING) {
      overTimer_ = 0.0f;
    }

    // -------------------------------------------------------------------------
    // 7. Command
    // -------------------------------------------------------------------------
    float cmd = pHold_ + pTerm + integral_;
    if (in.shadow) {                                  // the vent line holds the heater at 0 %: nothing else decides
      cmd = 0.0f;
      state_ = StFrozen;
    } else if (doorDetected_) {
      cmd = std::min(cmd, pHold_ + 6.0f);
      state_ = StDoor;
    } else if (sp_ - pv_ > 1.2f && filteredSlope_ >= 0.0f) {
      state_ = StClimb;
    } else {
      state_ = StTrack;
    }
    cmd = std::max(0.0f, std::min(capPct_, cmd));

    // -------------------------------------------------------------------------
    // 8. Command history (1 Hz): the pipe of the energy in flight and the record the heater-gain window reads
    // -------------------------------------------------------------------------
    secAcc_ += dt;
    if (secAcc_ >= 1.0f) {
      secAcc_ -= 1.0f;
      // A heater held off by the vent line is not a command the model should count as missing energy: it records the hold level.
      pushPipe(static_cast<uint8_t>(std::round(in.shadow ? pHold_ : cmd)));
      pushHist(pv_, in.shadow ? 0.0f : cmd);
      updateShortSlope();
    }

    // -------------------------------------------------------------------------
    // 9. Safety watchdog: a lot of power for a long time without PV answering
    // -------------------------------------------------------------------------
    if (cmd > 40.0f) {
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

  // Telemetry readouts for machine supervision and web monitors
  float predicted() const { return pvPredicted_; }
  uint8_t state() const { return state_; }
  uint8_t phase() const { return (state_ == StTrack || state_ == StDoor) ? 2U : (state_ == StClimb ? 1U : 0U); }
  bool responseSuspect() const { return respSuspect_; }
  bool identified() const { return identified_; }
  uint8_t batchesDone() const { return identified_ ? 2U : 0U; }
  float kAir() const { return kAir_; }
  float tauTransit() const { return tauTransit_; }
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

  // Slope of the last SLOPE_WINDOW_S seconds of PV, once per second (least squares on the 1 Hz history).
  void updateShortSlope() {
    if (histCount_ < SLOPE_WINDOW_S) { slopeShort_ = 0.0f; return; }
    const float xm = 0.5f * static_cast<float>(SLOPE_WINDOW_S - 1U);
    const float ref = pvAge(0U);                                        // differences to the newest value: a 37 degC offset would cost float precision
    float sxy = 0.0f;
    for (uint16_t i = 0U; i < SLOPE_WINDOW_S; ++i) sxy += (static_cast<float>(i) - xm) * (pvAge(static_cast<uint16_t>(SLOPE_WINDOW_S - 1U - i)) - ref);
    const float sxx = static_cast<float>(SLOPE_WINDOW_S) * (static_cast<float>(SLOPE_WINDOW_S) * static_cast<float>(SLOPE_WINDOW_S) - 1.0f) / 12.0f;
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
  float dQ_ = 0.1f;

  float kAir_ = 0.0004f;
  float tauTransit_ = 150.0f;
  float pHold_ = 18.0f;
  float integral_ = 0.0f;
  float pvPredicted_ = 30.0f;

  uint16_t pipeLen_ = 202U;
  uint16_t pipeIdx_ = 0U;
  uint8_t pipe_[MAX_PIPE_LEN] = {};
  int32_t pipeSum_ = 0;

  uint16_t histHead_ = 0U;
  uint16_t histCount_ = 0U;
  float pvHist_[HIST_LEN] = {};
  uint8_t uHist_[HIST_LEN] = {};
  float lastCmd_ = 0.0f;

  float secAcc_ = 0.0f;
  double clockS_ = 0.0;          // seconds of control time: a float stops counting after about 20 days of uptime
  double heaterOnTime_ = -1.0;
  float pvAtOn_ = NAN;
  bool identified_ = false;

  float filteredSlope_ = 0.0f;
  float slopeShort_ = 0.0f;
  float peakClimbSlope_ = 0.0f;
  uint8_t slopeHistCount_ = 0U;
  uint8_t slopeHistHead_ = 0U;
  float slopePvHist_[SLOPE_HIST_CAP] = {};
  double slopeTimeHist_[SLOPE_HIST_CAP] = {};

  bool doorDetected_ = false;
  float doorTimer_ = 0.0f;
  bool doorArmed_ = true;
  float doorCalmS_ = 0.0f;
  float stuckTimer_ = 0.0f;
  float overTimer_ = 0.0f;

  bool respSuspect_ = false;
  float respHighPwrTimer_ = 0.0f;
  float respPvStart_ = NAN;
};

}  // namespace thermalv3
