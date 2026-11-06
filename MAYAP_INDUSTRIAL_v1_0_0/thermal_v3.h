#pragma once
// =========================================================================================
// Thermal core 3.5 - one body, no tuning constants. Pure C++11, header-only.
//
// The oven is an integrating process with a transport delay:   d(PV)/dt = kAir * (u(t - tau) - hold - fan)
//   kAir [degC/s per %] heater gain, tau [s] transport delay, hold [%] the power that keeps PV flat (loss + eggs), fan [%] what the fan takes out.
//
// Rule of this file: a threshold is DERIVED where it can be. Everything below comes from five measured numbers:
//   kAir, tau, hold  (identified while the controller runs; the machine keeps kAir and tau across a restart: restore(); hold is read again),  cap (the heater ceiling),  sigma (the noise of PV, measured).
//   inflightMax = kAir * (cap - hold) * tau     the most heat that can be on its way: degC that full power puts into the pipe
//   far         = inflightMax / 2               landing starts when the PREDICTED error is under this: the braking distance of THIS oven
//   near        = 0.3 * far
//   kn          = 1 / (2 * kAir * 1.35 tau)     the gain next to SV (SIMC for an integrator with a delay)
// The typed numbers are the priors (kAir, tau, hold) a never-measured oven starts from, their plausibility range, and the few factors listed below with
// the measurement that fixed each.
//
//   power  u = hold + f(el),  el = (SV - PV) - inflight,  inflight = kAir * sum over the last tau seconds of (u - hold)
//   f: full power while el >= far (a door, cold eggs, a start), linear landing down to kn * near at el = near, kn * el below, 3 kn * el above SV.
//   hold has ONE writer (observe): a disturbance observer on the measured PV (see there).
//   kAir is measured at every saturated climb, tau by the stopwatch of a clean start.
//   The exhaust fan: the controller is told only that it runs, and holds the load estimate meanwhile. The vent line (vent_line.h) alone decides when the fan
//   runs (over temperature, a minimum run so the contactor is not burnt, off again at SV).
// What the machine does around the controller (protections, vent line) can only cut; the one rule that reaches into the state: a protection cut
// over SV takes the power that made that overshoot out of hold.
// =========================================================================================
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace thermalv3 {

// Priors of an oven nobody has measured (this machine's own: first climb 0.39 degC/min at 78 % -> 8.3e-5; log of 02/11 -> 5e-5..1e-4) and the range
// a measurement may take relative to them (a physical plausibility guard, not a tuning).
constexpr float KAIR_PRIOR = 8.0e-5f, KAIR_LO = 1.0f / 3.0f, KAIR_HI = 10.0f;
constexpr float TAU_PRIOR = 150.0f, TAU_LO = 0.6f, TAU_HI = 1.5f;
constexpr float KAIR_WARM = 2.5e-4f;       // a start close to SV has no climb to measure kAir on: the middle of the fleet instead of its slowest oven (gain safe on both sides)
constexpr float OBS_POLE_TAU = 1.5f, OBS_LEAK_TAU = 8.0f;   // disturbance observer: poles at 1/(1.5 tau); the rate of the load leaks away over 8 tau (measured on the fleet bench)
constexpr float PIPE_K = 1.3f;             // heat in flight: the last 1.3 transit times of power (the delay and the tail of the first-order lag behind it)
constexpr float LEAD_GAIN = 1.4f;          // ... at kAir x 1.4: the climb slope is what the eggs and walls leave of the air's own rate (A7 of the rig: 3.9e-4 against Pmax/Cf = 5.3e-4), and the heat in flight raises the air at that rate (closed-loop rig, door 2 min: overshoot 0.48 -> <= 0.30)
constexpr float KAIR_START = 1.6e-4f;      // kAir before any climb has been measured: between the slowest oven of the fleet and the middle (higher starts overshoot, lower ones undershoot)
constexpr float DOOR_LATCH_TAU = 6.0f;      // a door is "open" for the load estimate for at most 6 transit times
constexpr float DOOR_LOAD = 0.1f;           // ... and only when the fall is worth a load of 10 % of the headroom (a real door is 5-10 times that; the dip of a landing is not)
constexpr float HOLD_PRIOR_OF_CAP = 0.12f;
// Hold level under which the oven has more heat than it loses (a hot noon, the eggs' own heat): 1-3 % of the heater or nothing changes the temperature by
// less than the sensor can tell, but it keeps the reading on the wrong side of SV. PV ZERO_ZONE_ENTER_C over SV with the hold estimate under ZERO_ZONE_HOLD_PCT: the heater
// is simply off until PV is ZERO_ZONE_EXIT_C under SV (or the estimate rose over ZERO_ZONE_RELEASE_HOLD_PCT). Rig of the log of 05/11 (tests/thermal-closed-loop.cpp,
// noon scenarios): time over SV+0.05 drops strongly; a threshold of 5 % or more already undershoots (-0.12), so the zone stays at 3 %.
constexpr float ZERO_ZONE_HOLD_PCT = 3.0f, ZERO_ZONE_RELEASE_HOLD_PCT = 4.5f, ZERO_ZONE_ENTER_C = 0.05f, ZERO_ZONE_EXIT_C = 0.02f;
// The proportional action does nothing for a predicted error of less than 0.03 degC either side of SV (hold stays): the fine PV carries about 0.03 degC of noise and
// without a dead zone the request dithers by +-1 point on it (rig, steady hold: mean |request(t+60 s) - request(t)| 0.17 -> 0.04 points with the default gain, 0.42 -> 0.09
// with the gain measured on the oven; direction reversals 1.7 -> 0 per hour; offset unchanged). A SHIFTED dead band (error - 0.03) was tried and dropped: +0.02 overshoot on the
// first climb and 127 s slower set-point step on the closed-loop family; the zone without a shift costs neither.
constexpr float PROP_DEADBAND_C = 0.03f;
#ifndef LIFT_TI_TAU
#define LIFT_TI_TAU 1.0f     // stall lift (see stallLift): the shortfall of the measured rise is paid into the hold with an integral time of one transit time
#endif
#if !defined(LIFT_SLOPE) && !defined(LIFT_NO_SLOPE)
#define LIFT_SLOPE           // the slope form: only the measured rise counts (the older form read the gap minus the heat in flight and could not see a stall)
#endif
#ifndef TRIM_BAND_V
#define TRIM_BAND_V 0.15f
#endif
#ifndef TRIM_DEAD_V
#define TRIM_DEAD_V 0.02f
#endif
#ifndef TRIM_QUIET_V
#define TRIM_QUIET_V 1800.0f
#endif
constexpr float TRIM_BAND_C = TRIM_BAND_V, TRIM_DEAD_C = TRIM_DEAD_V, TRIM_QUIET_S = TRIM_QUIET_V;
constexpr float LIFT_TI_OF_TAU = LIFT_TI_TAU;
#ifndef LIFT_WAIT
#define LIFT_WAIT 2.0f
#endif
#ifndef LIFT_GATE
#define LIFT_GATE 0.2f
#endif
constexpr float LIFT_GATE_C = LIFT_GATE;
#ifndef LIFT_CLOSE
#define LIFT_CLOSE 4.0f
#endif
#ifndef LIFT_KREF_V
#define LIFT_KREF_V 1.2e-4f
#endif
constexpr float LIFT_CLOSE_TAU = LIFT_CLOSE, LIFT_KREF = LIFT_KREF_V;
constexpr float SIGMA_FLOOR = 0.004f;      // a reading of a 0.1 degC sensor cannot be quieter than this (degC)
constexpr uint16_t HIST_LEN = 640U;        // seconds of history (memory, not tuning): 4 x the largest transit time

enum State : uint8_t { StIdle = 0, StClimb = 1, StTrack = 2, StDoor = 3, StFrozen = 4 };

struct Input {
  float pv = NAN;                // PV the controller regulates [degC] (0.01 effective)
  float sp = 37.5f;
  float dQ = 0.1f;               // unused (kept for the callers)
  float slopeFine = NAN;         // long-window slope of PV [degC/s]; NaN: the controller reads its own
  float capPct = 100.0f;
  float deliveredPct = 0.0f;     // unused
  float dtS = 0.1f;
  bool newSample = false;        // unused
  bool permit = false;
  bool shadow = false;           // vent line holds the heater at 0 %
  bool ventFan = false;          // the exhaust fan runs (the only thing the controller is told about it)
  float headroomC = 0.7f, alarmHeadroomC = 0.7f, e115Frac = 0.0f;   // unused
};

struct Output {
  float powerPct = 0.0f;
  uint8_t state = StIdle;
  bool powerLimited = false;
  uint8_t trustQ = 15;
};

class Controller {
 public:
  Controller() { reset(); }

  void reset() {
    state_ = StIdle; pv_ = NAN; sp_ = 37.5f; cap_ = 100.0f;
    kAir_ = KAIR_START; tau_ = TAU_PRIOR; hold_ = HOLD_PRIOR_OF_CAP * 100.0f;
    identified_ = tauMeasured_ = door_ = wasLive_ = doorLatch_ = nearSeen_ = kAirMeasured_ = false; latchT_ = 0.0f;
    lead_ = slope_ = lsSlope_ = lsSigma_ = 0.0f; sigma_ = 0.02f; stallS_ = lifted_ = 0.0f; lifting_ = false;
    histHead_ = histCount_ = 0U; liveS_ = secAcc_ = lastCmd_ = 0.0f;
    swOn_ = false; swPv0_ = swT_ = 0.0f;
    pm_ = NAN; rate_ = 0.0f; zz_ = false;
    derive();
    for (uint16_t i = 0U; i < HIST_LEN; ++i) { pvHist_[i] = 0.0f; uHist_[i] = 0.0f; }
  }

  Output step(const Input &in) {
    Output out;
    const float dt = !std::isfinite(in.dtS) ? 0.1f : (in.dtS < 0.001f ? 0.001f : (in.dtS > 1.0f ? 1.0f : in.dtS));
    const bool pvValid = std::isfinite(in.pv) && in.pv > 0.0f && in.pv < 60.0f;
    const bool spValid = std::isfinite(in.sp) && in.sp > 20.0f && in.sp < 50.0f;
    const bool live = (in.permit || in.shadow) && pvValid && spValid;
    cap_ = (std::isfinite(in.capPct) && in.capPct > 0.0f) ? std::min(100.0f, in.capPct) : 0.0f;

    if (!live || cap_ <= 0.0f) {
      state_ = StFrozen; out.state = state_;
      // A protection cut the heater while PV stood over SV: the power that made that overshoot comes out of hold.
      if (pvValid && spValid && cap_ > 0.0f && !in.permit && !in.shadow && wasLive_ && in.pv > in.sp + std::max(3.0f * sigma_, nearBand_)) {
        hold_ = std::max(0.0f, hold_ - (in.pv - in.sp) / std::max(1.0e-9f, kAir_ * tau_));
      }
      wasLive_ = false; liveS_ = lastCmd_ = 0.0f; swOn_ = false; zz_ = false;
      secAcc_ += dt;
      while (secAcc_ >= 1.0f) { secAcc_ -= 1.0f; pushHist(std::isfinite(pv_) ? pv_ : 0.0f, 0.0f); }
      return out;
    }
    // Started close to SV (a reboot, a restart): there is no climb to measure kAir on - begin at the middle of the fleet, not at its slowest oven.
    if (!wasLive_ && !identified_ && !tauMeasured_ && liveS_ <= 0.0f && std::fabs(in.sp - in.pv) < farBand_) kAir_ = KAIR_WARM;
    wasLive_ = true; pv_ = in.pv; sp_ = in.sp;
    slope_ = std::isfinite(in.slopeFine) ? in.slopeFine : lsSlope_;
    const float e = sp_ - pv_;

    // The exhaust fan needs no rule of its own: it is a load like any other and the observer below sees it (a separate fan model counted it twice).

    // --- power: hold plus a saturating function of the PREDICTED error (the heat on its way is counted before it arrives)
    ventFan_ = in.ventFan;   // the fan takes heat out for a few minutes and stops: that load is not a hold level to learn (the observer is held while it runs)
    updateInflight();
    const float el = e - lead_;
    elLast_ = el;
    const float span = std::max(1.0f, cap_ - hold_);
    float d;
    if (el >= farBand_) d = span;
    else if (el >= nearBand_) d = kn_ * nearBand_ + (span - kn_ * nearBand_) * (el - nearBand_) / std::max(1.0e-6f, farBand_ - nearBand_);
    else {
      const float elp = (el > -PROP_DEADBAND_C && el < PROP_DEADBAND_C) ? 0.0f : el;     // dead zone around SV (no shift: beyond it the law is unchanged)
      if (elp >= 0.0f) d = kn_ * elp;
      else d = std::max(-hold_, 3.0f * kn_ * elp);                       // above SV: three times the gain
    }
    float cmd = hold_ + d;
    if (!in.shadow) {                                                      // the zero zone (see ZERO_ZONE_HOLD_PCT)
      if (!zz_ && hold_ < ZERO_ZONE_HOLD_PCT && e <= -ZERO_ZONE_ENTER_C) zz_ = true;
      else if (zz_ && (hold_ > ZERO_ZONE_RELEASE_HOLD_PCT || e > ZERO_ZONE_EXIT_C)) zz_ = false;
      if (zz_) cmd = 0.0f;
    }
    if (in.shadow) cmd = 0.0f;
    cmd = std::max(0.0f, std::min(cap_, cmd));

    // --- hold's correction by the predicted error (inside the landing band only; the balance below is the other, slower, writer)

    // door / cold eggs: PV falls faster than half of what the hold power would lose, and by more than the noise of the slope
    door_ = !in.shadow && e > nearBand_ && lsSlope_ < -std::max(3.0f * lsSigma_, 0.5f * kAir_ * hold_);
    // The door counts as open for the load estimate until PV is back near SV, or DOOR_LATCH_TAU transit times have run out. door_ above clears as soon as the
    // heater has stopped the fall, with the door still open; the observer then took the door's loss for hold (18 -> 43 %) and the whole surplus arrived when the door
    // shut (closed-loop rig, door 5 min: overshoot 1.04 -> 0.20 degC; 10 min: 2.39 -> 0.11).
    if (e < 2.0f * farBand_) nearSeen_ = true;                       // a door opens on an oven that has been running near SV, not on a cold start
    if (door_ && latchT_ <= 0.0f && nearSeen_ && -lsSlope_ / kAir_ > DOOR_LOAD * std::max(1.0f, cap_ - hold_)) latchT_ = 1.0e-3f;
    if (latchT_ > 0.0f) { latchT_ += dt; if (e <= nearBand_) latchT_ = 0.0f; }
    doorLatch_ = latchT_ > 0.0f && latchT_ <= DOOR_LATCH_TAU * tau_;
    state_ = in.shadow ? StFrozen : (door_ ? StDoor : (e > farBand_ && slope_ >= 0.0f ? StClimb : StTrack));

    // --- once per second: history, noise, slope, balance, identification
    secAcc_ += dt; liveS_ += dt;
    if (secAcc_ >= 1.0f) {
      secAcc_ -= 1.0f;
      const float net = in.shadow ? 0.0f : cmd;
      pushHist(pv_, in.shadow ? 0.0f : net);
      measureNoiseAndSlope();
      derive();
      if (in.permit && !in.shadow) { observe(); stallLift(); trim(); identifyGain(e); } else if (in.shadow) observe();
      stopwatch(e, in.permit && !in.shadow);
    }
    lastCmd_ = in.shadow ? 0.0f : cmd;
    out.powerPct = cmd; out.state = state_; out.powerLimited = cmd >= cap_;
    out.trustQ = door_ ? 6U : (identified_ ? 15U : 10U);
    return out;
  }

  // Telemetry for the machine and the web.
  float predicted() const { return pv_ + lead_; }
  uint8_t state() const { return state_; }
  uint8_t phase() const { return (state_ == StTrack || state_ == StDoor) ? 2U : (state_ == StClimb ? 1U : 0U); }
  bool responseSuspect() const { return false; }
  bool identified() const { return identified_; }
  // kAir has been MEASURED by a climb since power-up (a number restored from the EEPROM or typed by hand does not count): only that is worth keeping.
  bool kAirMeasured() const { return kAirMeasured_; }
  bool tauMeasured() const { return tauMeasured_; }
  // The numbers that belong to the OVEN, not to a batch: the machine keeps what a climb measured across a restart (and the owner may type a value), so that a
  // start close to SV does not have to guess kAir (a guess 2.5 times off leaves the oven under SV for an hour). A value outside its plausible range is ignored;
  // tau 0 = unknown (the transit time stays a prior that the next clean climb measures again).
  void restore(float kAir, float tau) {
    if (std::isfinite(kAir) && kAir >= KAIR_PRIOR * KAIR_LO && kAir <= KAIR_PRIOR * KAIR_HI) { kAir_ = kAir; identified_ = true; }
    if (std::isfinite(tau) && tau >= TAU_PRIOR * TAU_LO && tau <= TAU_PRIOR * TAU_HI) tau_ = tau;
    derive();
  }
  // The hold level of the oven (percent of the heater) given back after a restart (thermal_hold_record.h decides whether and how much). A value outside what the
  // heater can give is ignored.
  void restoreHold(float hold) {
    if (std::isfinite(hold) && hold >= 0.0f && hold <= 40.0f) hold_ = hold;
    derive();
  }
  // The climb assist (thermal_climb.h) hands over the load it found: any level the heater can give, and the observer starts again from the PV that is there.
  void adoptHold(float hold, bool seedHistory = false) {
    if (std::isfinite(hold) && hold >= 0.0f) {
      hold_ = std::min(hold, cap_ > 0.0f ? cap_ : 100.0f); pm_ = pv_; rate_ = 0.0f;
      // The observer reads the power of one transit time ago. Right after a start that history is empty (zero power), and a hold level that was just given looks far too high
      // to it. Seeding says what the oven was getting: the power that holds SV (= the hold level) for the whole history, and a PV that has stood still.
      if (seedHistory && std::isfinite(pv_)) {
        for (uint16_t i = 0U; i < HIST_LEN; ++i) { uHist_[i] = std::max(0.0f, std::min(100.0f, hold_)); pvHist_[i] = pv_; }
        histCount_ = HIST_LEN;
      }
    }
    derive();
  }
  bool hasRun() const { return wasLive_ || liveS_ > 0.0f; }
  // PV close to SV, no door, tracking: the hold estimate is worth keeping.
  bool settled() const { return wasLive_ && state_ == StTrack && !doorLatch_ && std::isfinite(pv_) && std::fabs(sp_ - pv_) <= nearBand_; }
  bool zeroZone() const { return zz_; }
  uint8_t batchesDone() const { return identified_ ? 2U : 0U; }
  float kAir() const { return kAir_; }
  float tauTransit() const { return tau_; }
  uint16_t pipeLen() const { return static_cast<uint16_t>(std::max(15.0f, std::min(319.0f, tau_ * 1.35f))); }
  float tdHat() const { return tau_; }
  float tauHat() const { return hold_; }
  float pHold() const { return hold_; }
  float dNow() const { return hold_; }
  // The stall lift (stallLift), for the log: what it has paid into the hold and not given back [percent], how long the rise has been short [s], and whether it paid this second.
  float lifted() const { return lifted_; }
  float stallSeconds() const { return stallS_; }
  bool lifting() const { return lifting_; }
  float haPct() const { return 0.0f; }
  float sigmaPred() const { return sigma_; }
  float integral() const { return 0.0f; }
  float slopeMks() const { return slope_ * 1000.0f; }
  bool doorDetected() const { return door_; }

 private:
  // Every threshold from the measured numbers (see the header). Called once per second.
  void derive() {
    const float cap = cap_ > 0.0f ? cap_ : 100.0f;
    const float span = std::max(1.0f, cap - hold_);
    inflightMax_ = kAir_ * span * tau_;
    farBand_ = std::max(0.5f * inflightMax_, 6.0f * sigma_);
    nearBand_ = std::max(0.3f * farBand_, 3.0f * sigma_);
    farBand_ = std::max(farBand_, nearBand_ * 2.0f);
    const float siMc = 1.0f / (2.0f * kAir_ * 1.35f * tau_);
    kn_ = std::max(0.05f * span / farBand_, std::min(siMc, 0.5f * span / nearBand_));
  }
  void pushHist(float pv, float u) {
    pvHist_[histHead_] = pv; uHist_[histHead_] = std::max(0.0f, std::min(100.0f, u));
    histHead_ = static_cast<uint16_t>((histHead_ + 1U) % HIST_LEN);
    if (histCount_ < HIST_LEN) ++histCount_;
  }
  float pvAge(uint16_t a) const { return pvHist_[(histHead_ + HIST_LEN - 1U - a) % HIST_LEN]; }
  float uAge(uint16_t a) const { return uHist_[(histHead_ + HIST_LEN - 1U - a) % HIST_LEN]; }
  // Noise and slope of PV from the last 0.8 tau seconds: least-squares line, its residual (sigma) and the uncertainty of its slope.
  void measureNoiseAndSlope() {
    const uint16_t n = static_cast<uint16_t>(std::min<float>(std::max(30.0f, 0.8f * tau_), static_cast<float>(HIST_LEN - 1U)));
    if (histCount_ < n || liveS_ < static_cast<float>(n)) { lsSlope_ = 0.0f; lsSigma_ = sigma_; return; }
    const float xm = 0.5f * static_cast<float>(n - 1U), ref = pvAge(0U);
    float sxy = 0.0f;
    for (uint16_t i = 0U; i < n; ++i) sxy += (static_cast<float>(i) - xm) * (pvAge(static_cast<uint16_t>(n - 1U - i)) - ref);
    const float sxx = static_cast<float>(n) * (static_cast<float>(n) * static_cast<float>(n) - 1.0f) / 12.0f;
    const float b = sxy / sxx;
    float ss = 0.0f;
    // residual about the fitted line (with its own intercept)
    float ym = 0.0f;
    for (uint16_t i = 0U; i < n; ++i) ym += pvAge(i) - ref;
    ym /= static_cast<float>(n);
    for (uint16_t i = 0U; i < n; ++i) {
      const float x = static_cast<float>(i) - xm, y = pvAge(static_cast<uint16_t>(n - 1U - i)) - ref;
      const float r = y - (ym + b * x);
      ss += r * r;
    }
    const float sig = std::max(SIGMA_FLOOR, std::sqrt(ss / static_cast<float>(n - 2U)));
    sigma_ += 0.1f * (sig - sigma_);
    lsSlope_ = b;
    lsSigma_ = std::max(sigma_ * std::sqrt(1.0f / sxx), 1.0e-6f);
  }
  // Heat in flight: what was commanded in the last tau seconds above what the oven loses has not reached the air yet.
  void updateInflight() {
    const uint16_t n = static_cast<uint16_t>(std::min<float>(PIPE_K * tau_, static_cast<float>(histCount_)));
    float sum = 0.0f;
#ifdef LEAD_REF_OLDER
    // reference = what the air was getting BEFORE the window (when that history exists), else the hold estimate
    float ref = hold_;
    if (histCount_ >= static_cast<uint16_t>(3U * n) && n > 0U) { float a = 0.0f; for (uint16_t i = n; i < 3U * n; ++i) a += uAge(i); ref = a / static_cast<float>(2U * n); }
    for (uint16_t i = 0U; i < n; ++i) sum += uAge(i) - ref;
#else
    for (uint16_t i = 0U; i < n; ++i) sum += uAge(i) - hold_;
#endif
    float lead = kAir_ * LEAD_GAIN * sum;
    // On a climb (far under SV) the measured slope is a floor for the heat in flight (a wrong kAir cannot hide it); near SV its noise must not count.
    if (std::isfinite(pv_) && sp_ - pv_ > 1.5f * farBand_) lead = std::max(lead, 0.8f * std::max(0.0f, slope_) * tau_);
    lead_ = std::max(-kAir_ * hold_ * tau_, std::min(1.2f * inflightMax_, lead));
  }
  // THE writer of hold: a disturbance observer. A model of PV is driven by the power that has reached the oven (the command one transit time ago);
  // the error between the measured and the model PV corrects the model, the load (hold) and the load's rate of change. Three poles at 1/(1.5 tau)
  // (a ramp of load - cold eggs, the walls giving back heat - is followed without a standing error), the rate leaks away over 8 tau so that it does not
  // wander when nothing supports it. The estimate is held while the door is open or PV is far from SV (the load is then not what the error is about).
  void observe() {
    const uint16_t delay = static_cast<uint16_t>(tau_ + 0.5f);
    if (histCount_ < static_cast<uint16_t>(delay + 2U) || !std::isfinite(pv_)) { pm_ = pv_; return; }
    if (!std::isfinite(pm_) || door_ || doorLatch_ || std::fabs(sp_ - pv_) > 2.0f * farBand_) { pm_ = pv_; rate_ = 0.0f; return; }
    // While the exhaust fan runs the load is read as LESS, never as more: the fan takes heat out, so a reading that falls is no proof of a bigger load, but a
    // reading that stays over SV with the fan running IS proof of a smaller one (log of 05/11: with the estimate frozen under the fan it stood at 5 % for an
    // hour while the oven needed nothing). The trend (rate_) is kept for the same reason: wiped at every fan start, it never followed a falling load.
    const float holdBefore = hold_, rateBefore = rate_;
    const float u = uAge(delay);
    const float w = 1.0f / (OBS_POLE_TAU * tau_);
    const float err = pv_ - pm_;
    pm_ += kAir_ * (u - hold_) + 3.0f * w * err;
    const float rLim = cap_ / (2.0f * tau_);                      // the load cannot move faster than the whole ceiling in two transit times
    rate_ = (rate_ - (w * w * w / kAir_) * err) * (1.0f - 1.0f / (OBS_LEAK_TAU * tau_));
    rate_ = std::max(-rLim, std::min(rLim, rate_));
    hold_ = std::max(0.0f, std::min(cap_, hold_ + rate_ - (3.0f * w * w / kAir_) * err));
    if (ventFan_) { hold_ = std::min(hold_, holdBefore); rate_ = std::min(rate_, rateBefore); }
  }
  // PV stands under SV and the heat already on its way does not close the gap: the hold estimate is too low and the observer needs minutes to find it out
  // (a load of three times the usual hold took 50 minutes to climb 0.5 degC). The gap, as the proportional law sees it, is also paid into the hold
  // with an integral time of LIFT_TI_OF_TAU transit times; it stops by itself as soon as the heat in flight closes the gap.
  // Trim: near SV the observer follows a slowly changing load with a small standing error (an evening cooling: 0.04 degC under SV for half an hour). The error itself,
  // read on the fine PV, is paid into the hold with a long integral time (TRIM_TI_S) in BOTH directions; it needs no oven model.
  void trim() {
#ifdef TRIM_TI_S
    // quiet time after a change of the set-point: the climb to the new level is the hold law's job
    if (!std::isfinite(spTrim_) || std::fabs(sp_ - spTrim_) > 0.01f) { spTrim_ = sp_; quietTrimS_ = 0.0f; } else quietTrimS_ += 1.0f;
    if (quietTrimS_ < TRIM_QUIET_S) return;
    if (ventFan_ || door_ || doorLatch_) return;
    const float e = sp_ - pv_;
    // only UNDER SV: a load that grows is followed faster, a load that falls (a hot noon) is left to the observer, which must not be pulled down by the sensor step
    if (!std::isfinite(e) || e > TRIM_BAND_C || e <= TRIM_DEAD_C) return;
    hold_ = std::min(cap_, hold_ + kn_ * (e - TRIM_DEAD_C) / static_cast<float>(TRIM_TI_S));
#endif
  }
  void stallLift() {
    lifting_ = false;
    // GIVE-BACK: what was paid in is returned when PV passes SV (the overshoot is that surplus), over about one transit time; the rest is forgotten slowly
    if (lifted_ > 0.0f) {
      const float over = pv_ - sp_;
      if (over > 0.03f) { const float g = std::min(lifted_, std::max(lifted_ / tau_, 0.0f)); hold_ = std::max(0.0f, hold_ - g); lifted_ -= g; }
      else lifted_ *= (1.0f - 1.0f / (10.0f * tau_));
    }
    const float gate = std::max(LIFT_GATE_C, 3.0f * sigma_);
    const float gap = sp_ - pv_;
    if (ventFan_ || door_ || doorLatch_ || !std::isfinite(gap) || !(gap > gate && gap < farBand_)) { stallS_ = 0.0f; return; }
#ifdef LIFT_SLOPE
    // measured response only: PV must close the gap in LIFT_CLOSE_TAU transit times; what is missing of that slope is paid into the hold with the gain of a slow oven
    const float want = (sp_ - pv_) / (LIFT_CLOSE_TAU * tau_);
    const float have = std::max(0.0f, lsSlope_);
    if (have >= want) return;
    // STALL GATE: pay only when the shortfall has lasted LIFT_WAIT_TAU transit times in a row (a climb that is merely slow is left to the base law)
    if (have >= want) { stallS_ = 0.0f; return; }
    stallS_ += 1.0f;
    if (stallS_ < LIFT_WAIT * tau_) return;
    const float pay = std::min(cap_ - hold_, (want - have) / (LIFT_KREF * LIFT_TI_OF_TAU * tau_));
    hold_ += pay; lifted_ += pay; lifting_ = pay > 0.0f;
#else
    if (elLast_ > gate) hold_ = std::min(cap_, hold_ + kn_ * (elLast_ - gate) / (LIFT_TI_OF_TAU * tau_));
#endif
  }
  // kAir at every saturated climb: the heater has been at its ceiling for a whole transit time plus a part of one, PV is far under SV and climbing.
  void identifyGain(float e) {
    if (e < farBand_ || lsSlope_ <= 3.0f * lsSigma_) { epMax_ = 0.0f; epFresh_ = true; return; }
    const uint16_t need = static_cast<uint16_t>(1.4f * tau_);
    if (histCount_ < need) return;
    for (uint16_t i = 0U; i < need; ++i) if (uAge(i) < 0.9f * cap_) return;
    // the slope of a saturated climb is still rising while the window runs: the PEAK of the slope over the episode is the oven's gain
    const float obs = std::max(lsSlope_, slope_) / std::max(1.0f, cap_ - hold_);
    epMax_ = std::max(epMax_, obs);
    const float k = std::max(KAIR_PRIOR * KAIR_LO, std::min(KAIR_PRIOR * KAIR_HI, epMax_));
    kAir_ = identified_ && !epFresh_ ? kAir_ + 0.3f * (k - kAir_) : k;
    epFresh_ = false;
    identified_ = true;
    kAirMeasured_ = true;
  }
  // (The stopwatch starts as soon as full power does: waiting 0.4 tau first measured the delay short by that wait and clamped slow ovens at the lower
  // limit - door recoveries overshot by 1.3 degC; fleet of the closed-loop rig, door 2 min: worst overshoot 1.27 -> 0.48.)
  // Transit time by stopwatch on a clean start: full power from a PV that is not moving, far under SV, until PV has risen by what the noise allows to trust.
  void stopwatch(float e, bool ok) {
    if (tauMeasured_ || !ok) { swOn_ = false; return; }
    if (!swOn_) {
      if (e >= farBand_ && lastCmd_ >= 0.8f * cap_ && std::fabs(lsSlope_) <= 3.0f * lsSigma_ && liveS_ > 5.0f) { swOn_ = true; swPv0_ = pv_; swT_ = 0.0f; }
      return;
    }
    swT_ += 1.0f;
    if (lastCmd_ < 0.4f * cap_) { swOn_ = false; return; }
    if (pv_ - swPv0_ >= std::max(0.1f, 8.0f * sigma_)) {
      tau_ = std::max(TAU_PRIOR * TAU_LO, std::min(TAU_PRIOR * TAU_HI, swT_ * 1.05f)); tauMeasured_ = true; swOn_ = false;
    }
  }
  uint8_t state_ = StIdle;
  float pv_ = NAN, sp_ = 37.5f, cap_ = 100.0f;
  float kAir_ = KAIR_PRIOR, tau_ = TAU_PRIOR, hold_ = 12.0f;
  float epMax_ = 0.0f; bool epFresh_ = true; float latchT_ = 0.0f; bool doorLatch_ = false, nearSeen_ = false; float pm_ = NAN, rate_ = 0.0f;
  bool ventFan_ = false, zz_ = false;
  float elLast_ = 0.0f, spTrim_ = NAN, quietTrimS_ = 0.0f;
  bool identified_ = false, tauMeasured_ = false, door_ = false, wasLive_ = false, kAirMeasured_ = false;
  bool lifting_ = false; float stallS_ = 0.0f, lifted_ = 0.0f; float lead_ = 0.0f, slope_ = 0.0f, lsSlope_ = 0.0f, lsSigma_ = 0.0f, sigma_ = 0.02f;
  float inflightMax_ = 1.0f, farBand_ = 0.5f, nearBand_ = 0.15f, kn_ = 30.0f;
  float liveS_ = 0.0f, secAcc_ = 0.0f, lastCmd_ = 0.0f;
  bool swOn_ = false; float swPv0_ = 0.0f, swT_ = 0.0f;
  float pvHist_[HIST_LEN] = {};
  float uHist_[HIST_LEN] = {};
  uint16_t histHead_ = 0U, histCount_ = 0U;
};

}  // namespace thermalv3
