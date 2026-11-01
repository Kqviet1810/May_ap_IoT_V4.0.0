#pragma once
// =========================================================================================
// Thermal controller of the MAYAP incubator (core of firmware 2.6.0; the file keeps its name and namespace).
// Pure C++11 (no Arduino dependencies), header-only.
//
// One structure, one memory of the load:
//
//     heater % = load + P(error) [+ fan bump]
//
//   load      an integral. It is the ONLY memory of what the oven needs to hold SV (an empty oven ~1 %, an oven with eggs 60-70 %). It runs
//             over the whole range under SV (a reading that stands 1.5 degC under SV climbs it in minutes, a reading that stands 0.1 degC
//             under SV climbs it too, slowly) on the error minus the coast, is paused while the heater is at its ceiling or the reading is
//             closing in on SV by itself, and is bled four
//             times faster than it is built while the reading is above SV and the heater is on.
//   P         gain 20 %/degC near SV, 50 %/degC far from it, 90 %/degC above it. The error it uses is the MEASURED one minus the coast: the
//             rise of the reading that is still to come = (measured slope over 60 s, above the quantisation of one sensor step) x the transit
//             time, plus the heat the heater has already sent and that has not arrived (heater gain x the commands of the last horizon that
//             are above the one that arrives now). The load of the oven is not in it: the measured slope says what the oven does with the
//             heat that arrives now. A slope that is not there brakes nothing.
//   fan bump  the day profile of the exhaust fans is known in advance (ventOnInS / ventOffInS). The extra heat that holds the reading while
//             the fans run is learned from the error of the running block (and one transit time after it) and is applied one transit time
//             BEFORE the next block starts and withdrawn one transit time before it ends.
//
// Kept from the controller it replaces: the stopwatch of the transit time on a real full-power start, the door guard (the heater is held at
// load + 6 % while the reading falls fast: a heater at full power would only warm the room), the frozen state while the permit is lost.
// Gone: the learned hold level and its energy-balance learner, the energy-in-flight prediction, the heater-gain identification, the
// rules that patched them (Anti-Stuck, hold fade, restart cuts, start by distance, probe).
// =========================================================================================
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace thermalv3 {

#ifndef TV_KP_NEAR
#define TV_KP_NEAR 20.0f
#endif
constexpr float KP_NEAR = TV_KP_NEAR;        // proportional gain [%/degC] when the (coast-corrected) error is at most KP_NEAR_E ...
#ifndef TV_KP_FAR
#define TV_KP_FAR 50.0f
#endif
constexpr float KP_FAR = TV_KP_FAR;         // ... and when it is at least KP_FAR_E (linear in between)
constexpr float KP_NEAR_E = 0.3f;
constexpr float KP_FAR_E = 1.0f;
#ifndef TV_KP_DOWN
#define TV_KP_DOWN 90.0f
#endif
constexpr float KP_DOWN = TV_KP_DOWN;        // above SV: strong damping
#ifndef TV_KI_TI_H
#define TV_KI_TI_H 3.0f
#endif
constexpr float KI_TI_H = TV_KI_TI_H;      // integral time = this many horizons (the integral rate is the near-SV gain / integral time)
constexpr float KP_MIN = 4.0f;
#ifndef TV_KI_FAR_BOOST
#define TV_KI_FAR_BOOST 2.0f
#endif
constexpr float KI_FAR_BOOST = TV_KI_FAR_BOOST;    // the integral is this much faster for an error of KP_FAR_E and more (a reading that stands far under SV)
constexpr float ON_WAY_TAU = 4.0f;      // no integration while the present slope would reach SV within this many transit times
#ifndef TV_LOAD_PRIOR
#define TV_LOAD_PRIOR 12.0f
#endif
constexpr float LOAD_PRIOR_PCT = TV_LOAD_PRIOR;   // the load of a controller that starts next to SV with nothing remembered (a restart in the middle of a batch)
#ifndef TV_FLIGHT_UNKNOWN_W
#define TV_FLIGHT_UNKNOWN_W 0.2f
#endif
constexpr float FLIGHT_UNKNOWN_W = TV_FLIGHT_UNKNOWN_W;
#ifndef TV_KP_FAR_MIN
#define TV_KP_FAR_MIN 0.0f
#endif
constexpr float KP_FAR_MIN = TV_KP_FAR_MIN;   // lowest far-field gain: far from SV the in-flight brake decides when to stop, not a small gain
#ifndef TV_STALL_BOOST
#define TV_STALL_BOOST 1.0f
#endif
constexpr float STALL_BOOST = TV_STALL_BOOST;
constexpr float STALL_COAST_C = 0.1f;
#ifndef TV_LOAD_PRIOR_FAR
#define TV_LOAD_PRIOR_FAR 1
#endif
constexpr bool LOAD_PRIOR_FAR = TV_LOAD_PRIOR_FAR != 0;
#ifndef TV_SLOPE_K
#define TV_SLOPE_K 1.7f
#endif
constexpr float SLOPE_K = TV_SLOPE_K;       // the rise still to come from the present slope = slope x transit time x this (heater tail, sensor lag)
#ifndef TV_COAST_INT_MAX_C
#define TV_COAST_INT_MAX_C 0.25f
#endif
constexpr float COAST_INT_MAX_C = TV_COAST_INT_MAX_C;   // no integration while this much rise is still to come
#ifndef TV_COAST_FILTER_S
#define TV_COAST_FILTER_S 30.0f
#endif
constexpr float COAST_FILTER_S = TV_COAST_FILTER_S; // the coast is released this slowly: it is fed by the heater's own commands, a fast loop would make the heater pulse
#ifndef TV_KI_DOWN_MULT
#define TV_KI_DOWN_MULT 8.0f
#endif
constexpr float KI_DOWN_MULT = TV_KI_DOWN_MULT;    // a load that is too big overshoots, one that is too small is only slow
constexpr float BRAKE_WINDOW_S = 60.0f; // slope window of the coast [s]
constexpr float DOOR_WINDOW_S = 20.0f;  // slope window of the door guard [s]
constexpr float COAST_MAX_C = 3.0f;
constexpr float COAST_MIN_C = -0.2f;    // a fall of the reading or a heater that was backed off may lift the error by this much, no more
constexpr float KAIR_PRIOR = 4.0e-4f;   // heater gain of an oven nobody has measured [degC/s per %]: on the fast side, where a wrong number only brakes more
constexpr float KAIR_MIN = 5.0e-5f;     // [a 20 kW heater on 1.8 MJ/K is 1.1e-4, on 0.3 MJ/K it is 6.7e-4]
constexpr float KAIR_MAX = 7.5e-4f;
constexpr float HORIZON_K = 1.35f;      // the heat sent is felt over 1.35 transit times (transit + heater lag)
constexpr float FAN_BUMP_MAX_PCT = 40.0f;
constexpr float FAN_KI_MULT = 3.0f;     // the bump is learned faster than the load: a block lasts a few minutes
constexpr float TAU_MIN_S = 90.0f;      // the transit time is never taken shorter than this: a stopwatch that ran while the energy of an earlier
constexpr float TAU_MAX_S = 220.0f;     // command was still on its way reads too short, and a delay that is too short makes every brake too weak
constexpr float TAU_PRIOR_S = 150.0f;
constexpr float IDENT_MIN_ERR_C = 2.0f; // the transit time is timed only on a start at least this far under SV ...
constexpr float IDENT_MAX_SLOPE = 0.002f; // ... from a reading that is not moving [degC/s]
constexpr float DOOR_MAX_S = 300.0f;    // the door guard holds the heater back for at most this long per event
constexpr float DOOR_REARM_SLOPE = -0.006f;
constexpr uint16_t HIST_S = 128U;       // seconds of reading history
constexpr uint16_t U_HIST = 320U;       // seconds of command history: horizon (<= 1.35 x 220 s) + slope window

enum State : uint8_t {
  StIdle = 0,    // Standby / Not permitted to heat
  StClimb = 1,   // far under SV and not falling
  StTrack = 2,   // tracking
  StDoor = 3,    // door guard
  StFrozen = 4   // permit lost, vent-line hold, or no valid reading
};

struct Input {
  float pv = NAN;                // Filtered process temperature [degC]
  float sp = 37.5f;              // Target setpoint [degC]
  float dQ = 0.1f;               // Sensor resolution step (0.1 or 0.01) [degC]
  float capPct = 100.0f;         // Configured maximum heater ceiling [%]
  float dtS = 0.1f;              // Step interval [seconds]
  bool newSample = false;        // New valid sensor sample available (unused: the history is taken once a second)
  bool permit = false;           // Production safety permit (all protections green)
  bool shadow = false;           // Vent-line hold of the heater (keep state alive, command 0 %)
  bool ventFan = false;          // Exhaust/ventilation fan is running
  float ventOnInS = 1.0e6f;      // Seconds until the next scheduled vent block starts (0 inside a block)
  float ventOffInS = 1.0e6f;     // Seconds until the running / next scheduled block ends
};

struct Output {
  float powerPct = 0.0f;         // Commanded heater power [0.0 .. capPct]
  uint8_t state = StIdle;        // Controller operational state
  bool powerLimited = false;     // Power capped by ceiling or safety
  uint8_t trustQ = 15;           // Telemetry signal quality index (0..15)
};

class Controller {
 public:
  Controller() { reset(); }

  void reset() {
    state_ = StIdle;
    pv_ = NAN;
    sp_ = 37.5f;
    capPct_ = 100.0f;
    dQ_ = 0.1f;
    tauTransit_ = TAU_PRIOR_S;
    kAir_ = KAIR_PRIOR;
    peakSlope_ = 0.0f;
    inflight_ = 0.0f;
    fillCommands(0.0f);                       // before the first command the heater was idle: the first climb is judged against that
    identified_ = false;
    started_ = loadImported_ = kAirKnown_ = false;
    load_ = 0.0f;
    fanBump_ = 0.0f;
    pTerm_ = 0.0f;
    coast_ = 0.0f;
    wasBlock_ = false;
    tailS_ = 0.0f;
    histCount_ = 0U;
    histHead_ = 0U;
    for (uint16_t i = 0U; i < HIST_S; ++i) hist_[i] = 0.0f;
    slopeBrake_ = 0.0f;
    slopeDoor_ = 0.0f;
    slopeGain_ = 0.0f;
    secAcc_ = 0.0f;
    clockS_ = 0.0;
    liveS_ = 0.0f;
    lastCmd_ = 0.0f;
    heaterOnTime_ = -1.0;
    pvAtOn_ = NAN;
    doorDetected_ = false;
    doorTimer_ = 0.0f;
    doorArmed_ = true;
    doorCalmS_ = 0.0f;
  }

  Output step(const Input &in) {
    Output out;
    const float dt = !std::isfinite(in.dtS) ? 0.1f : (in.dtS < 0.001f ? 0.001f : (in.dtS > 1.0f ? 1.0f : in.dtS));
    clockS_ += static_cast<double>(dt);

    const bool pvValid = std::isfinite(in.pv) && in.pv > 0.0f && in.pv < 60.0f;
    const bool spValid = std::isfinite(in.sp) && in.sp > 20.0f && in.sp < 50.0f;
    const bool live = (in.permit || in.shadow) && pvValid && spValid;
    capPct_ = (std::isfinite(in.capPct) && in.capPct > 0.0f) ? std::min(100.0f, in.capPct) : 0.0f;
    dQ_ = (std::isfinite(in.dQ) && in.dQ >= 0.005f && in.dQ <= 0.2f) ? in.dQ : 0.1f;

    // The reading history (1 Hz) keeps running while the heater is held off: it is a measurement of the oven. A reading that is not valid
    // breaks it.
    secAcc_ += dt;
    if (pvValid) {
      while (secAcc_ >= 1.0f) { secAcc_ -= 1.0f; pushPv(in.pv); pushU(live ? lastCmd_ : 0.0f); updateSlopes(); }
    } else {
      histCount_ = 0U;
      slopeBrake_ = slopeDoor_ = slopeGain_ = 0.0f;
      if (secAcc_ > 1.0f) secAcc_ = 0.0f;
    }

    if (!live || capPct_ <= 0.0f) {
      state_ = StFrozen;
      out.powerPct = 0.0f;
      out.state = state_;
      liveS_ = 0.0f;
      lastCmd_ = 0.0f;
      coast_ = 0.0f;
      if (!identified_) heaterOnTime_ = -1.0;       // a pause breaks the stopwatch of the start
      return out;
    }

    pv_ = in.pv;
    sp_ = in.sp;
    if (!started_) {
      started_ = true;
      if (!loadImported_ && (sp_ - pv_ <= 3.0f || LOAD_PRIOR_FAR)) load_ = std::min(capPct_, LOAD_PRIOR_PCT);
    }
    liveS_ += dt;
    load_ = std::max(0.0f, std::min(capPct_, load_));

    // -------------------------------------------------------------------------
    // 1. Transit time: a stopwatch from "the heater is put to work at (nearly) full power on an oven that is far under SV and not moving" to
    //    the first 0.15 degC of rise. Only a real start is a clean measurement: a controller that restarts next to SV (an update, a power
    //    blip) or in the middle of a fall has nothing to time.
    // -------------------------------------------------------------------------
    if (!identified_) {
      if (heaterOnTime_ < 0.0) {
        if (in.permit && !in.shadow && (histCount_ < 20U || std::fabs(slopeDoor_) <= IDENT_MAX_SLOPE) && (sp_ - pv_) >= IDENT_MIN_ERR_C &&
            lastCmd_ >= 0.8f * capPct_) {
          heaterOnTime_ = clockS_;
          pvAtOn_ = pv_;
        }
      } else if (lastCmd_ < 0.4f * capPct_) {
        heaterOnTime_ = -1.0;                       // the heater was backed off: the stopwatch no longer times a full-power start
      } else if ((pv_ - pvAtOn_) >= 0.15f) {
        const float measuredDelay = static_cast<float>(clockS_ - heaterOnTime_);
        tauTransit_ = std::max(TAU_MIN_S, std::min(TAU_MAX_S, measuredDelay * 1.05f));
        identified_ = true;
      }
    }

    // Heater gain: the steepest 40 s slope seen while the heater really is at its ceiling. "Really": the power that reached the oven during
    // that slope, one transit time ago, was at least 90 % of the ceiling, so the slope is the answer of the oven to that power and to nothing
    // else (5 % of the power is taken for the losses of an oven that is a few degrees over the room).
    if (pv_ < sp_ - 1.0f && slopeGain_ > peakSlope_ && in.permit) {
      const float uWin = delayedMean(static_cast<uint16_t>(tauTransit_ + 0.5f), 40U);
      if (uWin >= 0.9f * capPct_) {
        peakSlope_ = slopeGain_;
        kAirKnown_ = true;
        kAir_ = std::max(KAIR_MIN, std::min(KAIR_MAX, peakSlope_ / std::max(10.0f, uWin - 5.0f)));
      }
    }

    // -------------------------------------------------------------------------
    // 2. Door guard: the reading falls fast while it is under SV. The heater is held at load + 6 % for the time the door is really open; the
    //    guard lets go when the fall stops, and in any case after DOOR_MAX_S (a cold tray keeps pulling the air down long after the door is
    //    shut). After a time-out it is armed again only once the fall has slowed.
    // -------------------------------------------------------------------------
    if (slopeDoor_ > DOOR_REARM_SLOPE) doorCalmS_ += dt; else doorCalmS_ = 0.0f;
    if (!doorArmed_ && !doorDetected_ && doorCalmS_ >= 20.0f) doorArmed_ = true;
    if (doorArmed_ && slopeDoor_ < -0.012f && (sp_ - pv_) > 0.40f) {
      doorDetected_ = true;
      doorArmed_ = false;
      doorTimer_ = 0.0f;
    }
    if (doorDetected_) {
      doorTimer_ += dt;
      if ((slopeDoor_ > -0.003f && doorTimer_ > 15.0f) || doorTimer_ > DOOR_MAX_S) doorDetected_ = false;
    }

    // -------------------------------------------------------------------------
    // 3. The scheduled exhaust fans: a block is known ahead of time. Before it (one transit time) and inside it the learned bump is applied,
    //    in the last transit time of the block it is withdrawn; inside the block and one and a half transit times after it the error teaches
    //    the bump instead of the load.
    // -------------------------------------------------------------------------
    const bool sched = in.ventOnInS < 1.0e5f || in.ventOffInS < 1.0e5f;
    const bool inBlock = sched && in.ventOnInS <= 0.0f;
    const bool pre = sched && !inBlock && in.ventOnInS <= tauTransit_;
    if (wasBlock_ && !inBlock) tailS_ = 1.5f * tauTransit_;
    wasBlock_ = inBlock;
    tailS_ = std::max(0.0f, tailS_ - dt);
    const bool ending = inBlock && in.ventOffInS <= tauTransit_;
    const bool bumpOn = pre || (inBlock && !ending);
    const bool teachBump = inBlock || tailS_ > 0.0f;

    // -------------------------------------------------------------------------
    // 4. Control law
    // -------------------------------------------------------------------------
    const float e = sp_ - pv_;
    const float noise = dQ_ / BRAKE_WINDOW_S;                                  // one sensor step over the window is quantisation, not momentum
    const float sEff = slopeBrake_ > noise ? slopeBrake_ - noise : (slopeBrake_ < -noise ? slopeBrake_ + noise : 0.0f);
    // Near SV only what is MEASURED brakes (the slope): the heat in flight is the heater's own commands fed back, and next to SV that loop
    // would make the heater pulse. It joins in as the reading gets away from SV (the climb), where it is what keeps a fast oven from overshooting.
    const float wFlight = kAirKnown_ ? 1.0f : FLIGHT_UNKNOWN_W;   // a heater gain that was never measured is a guess on the fast side: it brakes less
    const float coastRaw = std::max(COAST_MIN_C, std::min(COAST_MAX_C, sEff * tauTransit_ * SLOPE_K + wFlight * inflight_));
    // The brake bites at once and lets go slowly: a fast oven must not be braked late, and the coast is fed by the heater's own commands.
    if (coastRaw > coast_) coast_ = coastRaw; else coast_ += (coastRaw - coast_) * std::min(1.0f, dt / COAST_FILTER_S);
    const float eEff = e - coast_;
    // The gains are sized to the oven: the loop gain (gain x heater gain x horizon) is about 0.5 near SV whatever the heater is. An oven that
    // answers fast after a long delay (a small air mass, a big heater) swung by +-1 degC under a fixed 30 %/degC.
    const float kAirH = kAir_ * static_cast<float>(horizon());
    const float kpNear = std::max(KP_MIN, std::min(KP_NEAR, 1.0f / (2.0f * kAirH)));
    const float kpFar = std::max(std::max(kpNear, KP_FAR_MIN), std::min(KP_FAR, 1.0f / kAirH));
    const float kpDown = std::max(12.0f, std::min(KP_DOWN, 3.0f * kpNear));
    const float ramp = std::max(0.0f, std::min(1.0f, (std::fabs(eEff) - KP_NEAR_E) / (KP_FAR_E - KP_NEAR_E)));
    const float eBelow = eEff + 0.5f * dQ_;                                    // above SV: half a sensor step is no overshoot
    pTerm_ = eEff >= 0.0f ? (kpNear + (kpFar - kpNear) * ramp) * eEff : (eBelow < 0.0f ? kpDown * eBelow : 0.0f);

    // An oven that has not been measured and stands far under SV is given the whole heater until its transit time is measured: the stopwatch
    // and the heater gain are read only from a start at (nearly) full power, and they are what every gain and every brake below is sized with.
    if (!identified_ && e > IDENT_MIN_ERR_C && !in.shadow) pTerm_ = std::max(pTerm_, capPct_);
    const float bump = bumpOn ? fanBump_ : 0.0f;
    const float uRaw = load_ + pTerm_ + bump;
    const float ki = kpNear / (KI_TI_H * static_cast<float>(horizon()));
    float dI = 0.0f;
    if (!in.shadow && !doorDetected_ && !pre) {
      if (eEff > 0.0f) {
        const bool saturated = uRaw >= capPct_;
        const bool onItsWay = coast_ > COAST_INT_MAX_C || (sEff > 0.0f && e / sEff < ON_WAY_TAU * tauTransit_);   // heat is on its way or the reading is closing in on SV by itself: the load is not what it lacks
        if (!saturated && !onItsWay) {
          const float far = std::max(0.0f, std::min(1.0f, (e - KP_NEAR_E) / (KP_FAR_E - KP_NEAR_E)));
          const float stall = coast_ < STALL_COAST_C ? STALL_BOOST : 1.0f;     // nothing on its way and still short: the load is what is missing
          dI = ki * (1.0f + (KI_FAR_BOOST - 1.0f) * far) * stall * eEff * dt;
        }
      } else if (eBelow < 0.0f && uRaw > 0.0f) {
        dI = ki * KI_DOWN_MULT * eBelow * dt;                                  // only while the heater is on: nothing to bleed otherwise
      }
    }
    if (teachBump) fanBump_ = std::max(0.0f, std::min(std::min(FAN_BUMP_MAX_PCT, capPct_), fanBump_ + dI * FAN_KI_MULT));
    else load_ = std::max(0.0f, std::min(capPct_, load_ + dI));

    float cmd = load_ + pTerm_ + bump;
    if (doorDetected_) {
      cmd = std::min(cmd, load_ + 6.0f);
      state_ = StDoor;
    } else if (in.shadow) {
      cmd = 0.0f;
      state_ = StFrozen;
    } else if (e > 1.2f && slopeDoor_ >= 0.0f) {
      state_ = StClimb;
    } else {
      state_ = StTrack;
    }
    cmd = std::max(0.0f, std::min(capPct_, cmd));

    lastCmd_ = in.shadow ? 0.0f : cmd;
    out.powerPct = cmd;
    out.state = state_;
    out.powerLimited = (cmd >= capPct_);
    out.trustQ = doorDetected_ ? 6U : (identified_ ? 15U : 10U);
    return out;
  }

  // Telemetry readouts for machine supervision and web monitors
  float predicted() const { return std::isfinite(pv_) ? pv_ + coast_ : pv_; }   // where the reading is going (measured slope x transit time)
  float coast() const { return coast_; }
  uint8_t state() const { return state_; }
  uint8_t phase() const { return (state_ == StTrack || state_ == StDoor) ? 2U : (state_ == StClimb ? 1U : 0U); }
  bool identified() const { return identified_; }
  uint8_t batchesDone() const { return identified_ ? 2U : 0U; }
  float tauTransit() const { return tauTransit_; }
  float kAir() const { return kAir_; }
  uint16_t horizonS() const { return horizon(); }
  float load() const { return load_; }
  float fanBump() const { return fanBump_; }
  float pTerm() const { return pTerm_; }
  float slopeMks() const { return slopeDoor_ * 1000.0f; }
  bool doorDetected() const { return doorDetected_; }

  // What an earlier run of THIS oven learned (see thermal_learn.h, which keeps it across a restart).
  // Heater gain and transit time are properties of the oven: they come back as the starting numbers of the next first climb, which measures
  // them again (identified_ stays false). The load is applied only to the batch that is being resumed (the same eggs, the same room); `measured` tells
  // that the transit time was measured on this oven.
  void importOven(float kAir, float tauTransit) {
    if (std::isfinite(kAir)) { kAir_ = std::max(KAIR_MIN, std::min(KAIR_MAX, kAir)); kAirKnown_ = true; }
    if (std::isfinite(tauTransit)) tauTransit_ = std::max(TAU_MIN_S, std::min(TAU_MAX_S, tauTransit));
  }
  void importLoad(float load, bool measured) {
    if (std::isfinite(load)) load_ = std::max(0.0f, std::min(100.0f, load));
    identified_ = measured;
    loadImported_ = true;
    fillCommands(load_);                       // the heater was at about the load before the restart: nothing is in flight
  }
  // A stored load that did not hold up (see MachineController::serviceThermalLearning): start the memory again from nothing.
  void forgetLoad() { load_ = 0.0f; fanBump_ = 0.0f; }

 private:
  void pushPv(float pv) {
    hist_[histHead_] = pv;
    histHead_ = static_cast<uint16_t>((histHead_ + 1U) % HIST_S);
    if (histCount_ < HIST_S) ++histCount_;
  }
  float pvAge(uint16_t age) const { return hist_[(histHead_ + HIST_S - 1U - age) % HIST_S]; }
  // Least-squares slope of the last w seconds of reading [degC/s]; 0 until the history reaches that far.
  float lsSlope(uint16_t w) const {
    if (histCount_ < w || w < 3U) return 0.0f;
    const float xm = 0.5f * static_cast<float>(w - 1U);
    const float ref = pvAge(0U);                                              // differences to the newest value keep the float precision
    float sxy = 0.0f;
    for (uint16_t i = 0U; i < w; ++i) sxy += (static_cast<float>(i) - xm) * (pvAge(static_cast<uint16_t>(w - 1U - i)) - ref);
    const float sxx = static_cast<float>(w) * (static_cast<float>(w) * static_cast<float>(w) - 1.0f) / 12.0f;
    return sxy / sxx;
  }
  void fillCommands(float u) {
    for (uint16_t i = 0U; i < U_HIST; ++i) uHist_[i] = u;
    uCount_ = U_HIST;
    uHead_ = 0U;
  }
  void pushU(float u) {
    uHist_[uHead_] = u;
    uHead_ = static_cast<uint16_t>((uHead_ + 1U) % U_HIST);
    if (uCount_ < U_HIST) ++uCount_;
  }
  float uAge(uint16_t age) const { return uHist_[(uHead_ + U_HIST - 1U - age) % U_HIST]; }
  uint16_t horizon() const {
    return static_cast<uint16_t>(std::max(15.0f, std::min(static_cast<float>(U_HIST - 2U), HORIZON_K * tauTransit_)));
  }
  // Mean command of `window` seconds that ended `delay` seconds ago; 0 when the history does not reach that far.
  float delayedMean(uint16_t delay, uint16_t window) const {
    if (uCount_ < static_cast<uint16_t>(delay + window)) return 0.0f;
    float sum = 0.0f;
    for (uint16_t i = 0U; i < window; ++i) sum += uAge(static_cast<uint16_t>(delay + i));
    return sum / static_cast<float>(window);
  }
  void updateSlopes() {
    slopeBrake_ = lsSlope(static_cast<uint16_t>(BRAKE_WINDOW_S));
    slopeDoor_ = lsSlope(static_cast<uint16_t>(DOOR_WINDOW_S));
    slopeGain_ = lsSlope(40U);
    // The heat that was sent and has not arrived: the commands of the last horizon above the one that arrives now. The slope is the mean of
    // the last 60 s, so "the one that arrives now" is the mean of the 60 s of commands that ended one horizon ago (a single command would make
    // the brake follow every pulse of the heater). Without a history that reaches back that far, what is known stands for it: nothing is in
    // flight that is not known.
    float sum = 0.0f;
    const uint16_t n = std::min<uint16_t>(horizon(), uCount_);
    for (uint16_t i = 0U; i < n; ++i) sum += uAge(i);
    const uint16_t w = std::min<uint16_t>(static_cast<uint16_t>(BRAKE_WINDOW_S), static_cast<uint16_t>(uCount_ - n));
    float arriving = load_;                    // no history that far back: what arrives is what holds the oven (nothing on a cold start)
    if (w > 0U) {
      arriving = 0.0f;
      for (uint16_t i = 0U; i < w; ++i) arriving += uAge(static_cast<uint16_t>(n + i));
      arriving /= static_cast<float>(w);
    }
    sum -= static_cast<float>(n) * arriving;
    inflight_ = kAir_ * sum;
  }

  uint8_t state_ = StIdle;
  float pv_ = NAN, sp_ = 37.5f, capPct_ = 100.0f, dQ_ = 0.1f;
  float tauTransit_ = TAU_PRIOR_S;
  bool identified_ = false;
  bool started_ = false, loadImported_ = false, kAirKnown_ = false;
  float load_ = 0.0f, fanBump_ = 0.0f, pTerm_ = 0.0f, coast_ = 0.0f;
  bool wasBlock_ = false;
  float tailS_ = 0.0f;
  float hist_[HIST_S] = {};
  uint16_t histCount_ = 0U, histHead_ = 0U;
  float slopeBrake_ = 0.0f, slopeDoor_ = 0.0f, slopeGain_ = 0.0f;
  float kAir_ = KAIR_PRIOR, peakSlope_ = 0.0f, inflight_ = 0.0f;
  float uHist_[U_HIST] = {};
  uint16_t uCount_ = 0U, uHead_ = 0U;
  float secAcc_ = 0.0f;
  double clockS_ = 0.0;
  float liveS_ = 0.0f;
  float lastCmd_ = 0.0f;
  double heaterOnTime_ = -1.0;
  float pvAtOn_ = NAN;
  bool doorDetected_ = false;
  float doorTimer_ = 0.0f;
  bool doorArmed_ = true;
  float doorCalmS_ = 0.0f;
};

}  // namespace thermalv3
