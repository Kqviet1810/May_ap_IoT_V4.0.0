#pragma once
// =========================================================================================
// Thermal core 3.5 - one body, no tuning constants. Pure C++11, header-only.
//
// The oven is an integrating process with a transport delay:   d(PV)/dt = kAir * (u(t - tau) - hold - fan)
//   kAir [degC/s per %] heater gain, tau [s] transport delay, hold [%] the power that keeps PV flat (loss + eggs), fan [%] what the fan takes out.
//
// Rule of this file: a threshold is DERIVED where it can be. Everything below comes from five measured numbers:
//   kAir, tau, hold  (identified while the controller runs; nothing is kept across a restart),  cap (the heater ceiling),  sigma (the noise of PV, measured).
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
constexpr float FAN_LEARN_GAIN = 0.7f;      // the share of the measured shortfall of a fan run that is taken into the fan's loss
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
  bool ventFan = false;          // the exhaust fan runs (any reason)
  float fanOnInS = NAN;          // the day profile of the fan: seconds until the next run starts (0 while it runs) ...
  float fanOffInS = NAN;         // ... and until that run ends; NaN when the fan has no schedule (a fan of the vent line is never fed forward)
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
    identified_ = tauMeasured_ = door_ = wasLive_ = doorLatch_ = nearSeen_ = false; latchT_ = 0.0f;
    lead_ = slope_ = lsSlope_ = lsSigma_ = 0.0f; sigma_ = 0.02f;
    histHead_ = histCount_ = 0U; liveS_ = secAcc_ = lastCmd_ = 0.0f;
    swOn_ = false; swPv0_ = swT_ = 0.0f;
    pm_ = NAN; rate_ = 0.0f; fanW_ = 0.0f; fanSched_ = fanSchedPrev_ = fanRun_ = false;
    derive();
    for (uint16_t i = 0U; i < HIST_LEN; ++i) { pvHist_[i] = 0.0f; uHist_[i] = 0.0f; ffHist_[i] = 0.0f; }
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
      wasLive_ = false; liveS_ = lastCmd_ = 0.0f; swOn_ = false;
      secAcc_ += dt;
      while (secAcc_ >= 1.0f) { secAcc_ -= 1.0f; pushHist(std::isfinite(pv_) ? pv_ : 0.0f, 0.0f, 0.0f); }
      return out;
    }
    // Started close to SV (a reboot, a restart): there is no climb to measure kAir on - begin at the middle of the fleet, not at its slowest oven.
    if (!wasLive_ && !identified_ && !tauMeasured_ && liveS_ <= 0.0f && std::fabs(in.sp - in.pv) < farBand_) kAir_ = KAIR_WARM;
    wasLive_ = true; pv_ = in.pv; sp_ = in.sp;
    slope_ = std::isfinite(in.slopeFine) ? in.slopeFine : lsSlope_;
    const float e = sp_ - pv_;

    // --- the exhaust fan. The fan of the day profile is known ahead: it takes fanW out of the oven for its whole run, and the heater needs one transit time
    // to answer, so the heater is given fanW from one transit time before the fan starts to one before it stops (the plant then sees no fan at all).
    // fanW is learned by the observer while that fan runs (it is the load the observer finds then; hold is what it finds with the fan off).
    // Any other fan (the vent line, over temperature) is not fed forward - it is there because the oven is too hot - and the observer is held while it runs.
    ventFan_ = in.ventFan;
    const bool haveSched = std::isfinite(in.fanOnInS) && std::isfinite(in.fanOffInS);
    fanSched_ = in.ventFan && haveSched && in.fanOnInS <= 0.5f;
    if (fanSched_ != fanSchedPrev_) { rate_ = 0.0f; fanSchedPrev_ = fanSched_; }
    float ff = 0.0f;
    if (haveSched && !in.shadow && fanW_ > 0.0f && in.fanOnInS <= tau_ && in.fanOffInS > tau_) ff = std::min(fanW_, 0.4f * cap_);

    // --- power: hold plus a saturating function of the PREDICTED error (the heat on its way is counted before it arrives)
    updateInflight();
    const float el = e - lead_;
    const float span = std::max(1.0f, cap_ - hold_);
    float d;
    if (el >= farBand_) d = span;
    else if (el >= nearBand_) d = kn_ * nearBand_ + (span - kn_ * nearBand_) * (el - nearBand_) / std::max(1.0e-6f, farBand_ - nearBand_);
    else if (el >= 0.0f) d = kn_ * el;
    else d = std::max(-hold_, 3.0f * kn_ * el);                           // above SV: three times the gain
    float cmd = hold_ + ff + d;
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
      pushHist(pv_, in.shadow ? 0.0f : net, in.shadow ? 0.0f : ff);
      measureNoiseAndSlope();
      derive();
      if (in.permit && !in.shadow) { observe(); identifyGain(e); measureFan(); } else if (in.shadow) observe();
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
  uint8_t batchesDone() const { return identified_ ? 2U : 0U; }
  float kAir() const { return kAir_; }
  float fanLoss() const { return fanW_; }
  bool tauMeasured() const { return tauMeasured_; }
  // The numbers that belong to the OVEN, not to a batch (what a climb measured, what the fan takes out): the machine keeps them across a restart so that a
  // start close to SV does not have to guess them. A value outside its plausible range is ignored; tau 0 = never measured.
  void restore(float kAir, float tau, float fanW) {
    if (std::isfinite(kAir) && kAir >= KAIR_PRIOR * KAIR_LO && kAir <= KAIR_PRIOR * KAIR_HI) { kAir_ = kAir; identified_ = true; }
    if (std::isfinite(tau) && tau >= TAU_PRIOR * TAU_LO && tau <= TAU_PRIOR * TAU_HI) tau_ = tau;   // a prior only: the next clean climb measures it again
    if (std::isfinite(fanW) && fanW >= 0.0f && fanW <= 40.0f) fanW_ = fanW;
    derive();
  }
  float tauTransit() const { return tau_; }
  uint16_t pipeLen() const { return static_cast<uint16_t>(std::max(15.0f, std::min(319.0f, tau_ * 1.35f))); }
  float tdHat() const { return tau_; }
  float tauHat() const { return hold_; }
  float pHold() const { return hold_; }
  float dNow() const { return hold_; }
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
  void pushHist(float pv, float u, float ff) {
    pvHist_[histHead_] = pv; uHist_[histHead_] = std::max(0.0f, std::min(100.0f, u)); ffHist_[histHead_] = ff;
    histHead_ = static_cast<uint16_t>((histHead_ + 1U) % HIST_LEN);
    if (histCount_ < HIST_LEN) ++histCount_;
  }
  float pvAge(uint16_t a) const { return pvHist_[(histHead_ + HIST_LEN - 1U - a) % HIST_LEN]; }
  float uAge(uint16_t a) const { return uHist_[(histHead_ + HIST_LEN - 1U - a) % HIST_LEN]; }
  float ffAge(uint16_t a) const { return ffHist_[(histHead_ + HIST_LEN - 1U - a) % HIST_LEN]; }
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
    for (uint16_t i = 0U; i < n; ++i) sum += uAge(i) - ffAge(i) - hold_;       // the fan's share is spent on the fan, not on the air
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
    if (!std::isfinite(pm_) || door_ || doorLatch_ || ventFan_ || std::fabs(sp_ - pv_) > 2.0f * farBand_) { pm_ = pv_; rate_ = 0.0f; return; }
    const float u = uAge(delay);
    const float w = 1.0f / (OBS_POLE_TAU * tau_);
    const float err = pv_ - pm_;
    pm_ += kAir_ * (u - hold_) + 3.0f * w * err;
    const float rLim = cap_ / (2.0f * tau_);                      // the load cannot move faster than the whole ceiling in two transit times
    rate_ = (rate_ - (w * w * w / kAir_) * err) * (1.0f - 1.0f / (OBS_LEAK_TAU * tau_));
    rate_ = std::max(-rLim, std::min(rLim, rate_));
    hold_ = std::max(0.0f, std::min(cap_, hold_ + rate_ - (3.0f * w * w / kAir_) * err));
  }
  // What the scheduled fan takes out, measured over its run: PV is compared with what the model says PV would do under the power that reached the oven with
  // the fan off (hold and the heater are known); the shortfall is the fan's loss, kAir x fanW x run time. Two readings of PV (the start and the end of the
  // run) and a model are all it needs, and the correction is a part of the difference so that one noisy run cannot spoil it.
  void measureFan() {
    const uint16_t delay = static_cast<uint16_t>(tau_ + 0.5f);
    if (fanSched_ && histCount_ > static_cast<uint16_t>(delay + 2U)) {
      if (!fanRun_) { fanRun_ = true; fanPv0_ = pv_; fanAcc_ = 0.0f; fanT_ = 0.0f; }
      fanAcc_ += kAir_ * (uAge(delay) - hold_ - fanW_);
      fanT_ += 1.0f;
    } else if (fanRun_) {
      fanRun_ = false;
      if (fanT_ >= 30.0f && !door_ && !doorLatch_) {
        const float miss = (pv_ - fanPv0_) - fanAcc_;                       // negative: the fan took more than fanW says
        fanW_ = std::max(0.0f, std::min(0.4f * cap_, fanW_ - FAN_LEARN_GAIN * miss / (kAir_ * fanT_)));
      }
    }
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
  bool ventFan_ = false, fanSched_ = false, fanSchedPrev_ = false, fanRun_ = false;
  float fanPv0_ = 0.0f, fanAcc_ = 0.0f, fanT_ = 0.0f;
  float fanW_ = 0.0f;                       // what the scheduled fan takes out of the oven [% of the heater]: learned while it runs, fed forward one transit time ahead
  bool identified_ = false, tauMeasured_ = false, door_ = false, wasLive_ = false;
  float lead_ = 0.0f, slope_ = 0.0f, lsSlope_ = 0.0f, lsSigma_ = 0.0f, sigma_ = 0.02f;
  float inflightMax_ = 1.0f, farBand_ = 0.5f, nearBand_ = 0.15f, kn_ = 30.0f;
  float liveS_ = 0.0f, secAcc_ = 0.0f, lastCmd_ = 0.0f;
  bool swOn_ = false; float swPv0_ = 0.0f, swT_ = 0.0f;
  float pvHist_[HIST_LEN] = {};
  float uHist_[HIST_LEN] = {};
  float ffHist_[HIST_LEN] = {};
  uint16_t histHead_ = 0U, histCount_ = 0U;
};

}  // namespace thermalv3
