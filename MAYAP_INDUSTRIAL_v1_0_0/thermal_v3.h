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
//   The exhaust fan: the controller is told that it runs. While the scheduled fan runs the load estimate is held; while a fan of the vent line runs (the oven is too hot) the
//   estimate may only come down (2.9.2). The vent line (vent_line.h) alone decides when the fan
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
// 2.9.2: the priors are THIS machine's, not the middle of an imagined fleet. Field: first climb 0.39 degC/min at 78 % -> 8.3e-5; 02/11 5e-5..1e-4; 03/11 9.4e-5 (so 9e-5);
// the transit time of the real oven is 50..130 s (log of 04/11), so 100 s. The 2.9.1 priors (KAIR_WARM 2.5e-4 near SV, tau 150 s) were 2.7 x too high on the gain
// (the proportional gain and the cut above SV 2.7 x too weak, 20+ minutes to close a 0.1 degC gap) and 1.5 x too long on the delay (the fan feed-forward arrived early).
// The stopwatch reads the time until PV has risen by 8 sigma, which is longer than the delay the control law needs (dead time + half the lag): on the rig it reads 90 / 102 / 188 / 222 s
// on ovens whose td + tl are 60 / 90 / 130 / 190 s, and a 222 s belief closes a 0.1 degC gap in 17 minutes where 100 s closes it in 6. A factor of 0.75 would be better on the fleet bench
// (96.7 % of the situations passed against 96.3 %) but the closed-loop suite (E115 on a healthy oven, overshoot of the empty oven) does not accept even 0.95: left at the 1.05 of 2.9.1.
constexpr float SW_TAU_FACTOR = 1.05f;
constexpr float STEADY_PIPE_FRAC = 0.1f;   // the command moved by less than this share of the ceiling over the pipe: the pipe is in steady state
constexpr float LEARN_STEP = 0.2f;          // the share of the way a passing fit of PlantLearner moves kAir and tau
constexpr float LEARN_BAND_C = 0.6f;        // ... fitted on seconds within this distance of SV only
constexpr float KAIR_PRIOR = 9.0e-5f, KAIR_LO = 0.2f, KAIR_HI = 10.0f;   // plausible gain 1.8e-5 .. 9e-4: a loaded oven of the field has 2.2e-5
constexpr float TAU_PRIOR = 100.0f, TAU_LO = 0.45f, TAU_HI = 2.25f;   // plausible transit time 45..225 s
constexpr float OBS_POLE_TAU = 1.5f, OBS_LEAK_TAU = 8.0f;   // disturbance observer: poles at 1/(1.5 tau); the rate of the load leaks away over 8 tau (measured on the fleet bench)
constexpr float PIPE_K = 1.3f;             // heat in flight: the last 1.3 transit times of power (the delay and the tail of the first-order lag behind it)
constexpr float LEAD_GAIN = 1.4f;          // ... at kAir x 1.4: the climb slope is what the eggs and walls leave of the air's own rate (A7 of the rig: 3.9e-4 against Pmax/Cf = 5.3e-4), and the heat in flight raises the air at that rate (closed-loop rig, door 2 min: overshoot 0.48 -> <= 0.30)
constexpr float DOOR_LATCH_TAU = 6.0f;      // a door is "open" for the load estimate for at most 6 transit times
constexpr float DOOR_LOAD = 0.1f;           // ... and only when the fall is worth a load of 10 % of the headroom (a real door is 5-10 times that; the dip of a landing is not)
constexpr float HOLD_PRIOR_OF_CAP = 0.12f;
constexpr float FF_LEAD_UNTRUSTED = 0.85f; // see step(): the lead of the fan feed-forward while the transit time is only the prior
constexpr float FF_BUMP_C = 0.05f;         // the feed-forward is cancelled when PV is this far over SV before the fan has started: the heat arrived early
constexpr float FAN_LEARN_GAIN = 0.7f;      // the share of the measured shortfall of a fan run that is taken into the fan's loss
constexpr float SIGMA_FLOOR = 0.004f;      // a reading of a 0.1 degC sensor cannot be quieter than this (degC)
constexpr uint16_t HIST_LEN = 640U;        // seconds of history (memory, not tuning): 4 x the largest transit time

// Continuous learning of the plant (2.9.2). The numbers of an oven are not constants: a loaded oven has a heater gain 4 x lower than an empty one (2.2e-5 against 9e-5 on the log
// of 04/11) and nothing but the machine can know which one it has today. Every 30 s the learner fits, over the last 10 minutes, the slope of PV to the command that reached the oven
// `delay` seconds earlier:  dPV/dt = kAir x u(t - delay) - c  (the intercept c is the hold, free), for delays of 45 .. 225 s, and takes the delay with the best fit. It learns from
// whatever the controller does - a climb, a landing, a swing of the command - and learns nothing (returns false) when the command did not move enough or the fit is poor. Seconds with
// a door, a fan or the heater off are left out. Fed once per second by the controller; also fed directly by the tests from a logged run.
class PlantLearner {
 public:
  static constexpr uint16_t N = 600U;            // seconds kept
  static constexpr uint16_t HALF = 15U;          // half of the smoothing window of the slope and of the command [s]
  void reset() { head_ = 0U; count_ = 0U; }
  void push(float pv, float u, bool clean) {
    pv_[head_] = pv; u_[head_] = u; ok_[head_] = clean; head_ = static_cast<uint16_t>((head_ + 1U) % N);
    if (count_ < N) ++count_;
  }
  // true when a trustworthy fit exists; kAir [degC/s per %], delay [s], r2 the share of the variance of the slope that the command explains
  bool fit(float &kAir, float &delay, float &r2out) const {
    if (count_ < N - 2U) return false;
    static const uint16_t MAXD = 225U;
    const uint16_t last = static_cast<uint16_t>(N - 1U - HALF);                    // newest second with a full smoothing window after it
    float bestR2 = -1.0f, bestK = 0.0f, bestD = 0.0f;
    for (uint16_t d = 45U; d <= MAXD; d = static_cast<uint16_t>(d + 15U)) {
      double sx = 0, sy = 0, sxx = 0, sxy = 0, syy = 0; uint16_t n = 0U;
      for (uint16_t i = static_cast<uint16_t>(d + HALF); i <= last; i = static_cast<uint16_t>(i + 5U)) {   // age-ordered from the oldest
        bool good = true; double ux = 0.0;
        for (uint16_t k = 0U; k <= 2U * HALF && good; k = static_cast<uint16_t>(k + 5U)) {
          const uint16_t a = static_cast<uint16_t>(i - d - HALF + k);               // command window, delay seconds earlier
          if (!ok_[at(a)]) good = false;
          ux += u_[at(a)];
        }
        if (!good || !ok_[at(i)] || !ok_[at(static_cast<uint16_t>(i - HALF))] || !ok_[at(static_cast<uint16_t>(i + HALF))]) continue;
        const double x = ux / 7.0, y = (pv_[at(static_cast<uint16_t>(i + HALF))] - pv_[at(static_cast<uint16_t>(i - HALF))]) / (2.0 * HALF);
        sx += x; sy += y; sxx += x * x; sxy += x * y; syy += y * y; ++n;
      }
      if (n < 40U) continue;
      const double vx = sxx / n - (sx / n) * (sx / n), vy = syy / n - (sy / n) * (sy / n), cxy = sxy / n - (sx / n) * (sy / n);
      if (vx < 64.0 || vy <= 0.0) continue;                                            // the command moved by less than 8 % (rms): nothing to learn from
      const double a = cxy / vx, r2 = (cxy * cxy) / (vx * vy);
      if (a > 0.0 && r2 > bestR2) { bestR2 = static_cast<float>(r2); bestK = static_cast<float>(a); bestD = static_cast<float>(d); }
    }
    if (bestR2 < 0.35f) return false;
    kAir = bestK; delay = bestD; r2out = bestR2;
    return true;
  }
 private:
  uint16_t at(uint16_t age_from_oldest) const { return static_cast<uint16_t>((head_ + N - count_ + age_from_oldest) % N); }
  float pv_[N] = {}, u_[N] = {};
  bool ok_[N] = {};
  uint16_t head_ = 0U, count_ = 0U;
};

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
    kAir_ = KAIR_PRIOR; tau_ = TAU_PRIOR; hold_ = HOLD_PRIOR_OF_CAP * 100.0f;
    identified_ = tauMeasured_ = tauTrusted_ = door_ = wasLive_ = doorLatch_ = nearSeen_ = false; latchT_ = 0.0f;
    lead_ = slope_ = lsSlope_ = lsSigma_ = 0.0f; sigma_ = 0.02f;
    histHead_ = histCount_ = 0U; liveS_ = secAcc_ = lastCmd_ = 0.0f; learner_.reset(); learnS_ = 0U;
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
      while (secAcc_ >= 1.0f) { secAcc_ -= 1.0f; pushHist(std::isfinite(pv_) ? pv_ : 0.0f, 0.0f, 0.0f); learner_.push(std::isfinite(pv_) ? pv_ : 0.0f, 0.0f, false); }
      return out;
    }
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
    // 2.9.2: the feed-forward leads the fan by the transit time ONLY when that was measured (this run's stopwatch, or the one the machine kept). With the prior it leads by 0.7 x
    // the prior: the real oven answers in 50..130 s, and 150 s arrived 50 s early (+0.1 degC before the fan and a deeper dip after it, 5 of 5 runs of the field log of
    // 04/11), while 85 s is early by at most 35 s on the fastest oven and late by at most 45 s on a 130 s one, and the feed-forward is cancelled as soon as PV shows the heat arrived
    // early (rig, td+tl 60..190 s: the dip of the fan 0.1 -> 0.02..0.07, bump <= +0.05 on the 50..130 s the field gives).
    const float ffLead = tauTrusted_ ? tau_ : FF_LEAD_UNTRUSTED * tau_;
    if (haveSched && !in.shadow && fanW_ > 0.0f && in.fanOnInS <= ffLead && in.fanOffInS > ffLead) ff = std::min(fanW_, 0.4f * cap_);
    if (ff > 0.0f && !in.ventFan && pv_ > sp_ + FF_BUMP_C) ff = 0.0f;       // the heat is already here and the fan is not: a transit time shorter than believed

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
    // 2.9.2: ... and faster than the model expects of the power that has reached the oven. After an early cut the oven coasts down at kAir x hold, which is the model's own
    // answer to the cut and not a door; taken for one it froze the load estimate for 6 transit times under a PV 0.5 - 2 degC low (rig, first climb; field log of 04/11).
    float expected = 0.0f;
    if (histCount_ > static_cast<uint16_t>(tau_ + 2.5f)) expected = kAir_ * (uAge(static_cast<uint16_t>(tau_ + 0.5f)) - hold_);
    door_ = !in.shadow && e > nearBand_ && lsSlope_ < -std::max(3.0f * lsSigma_, 0.5f * kAir_ * hold_) && lsSlope_ < expected - 0.5f * kAir_ * hold_;
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
      learner_.push(pv_, in.shadow ? 0.0f : net, in.permit && !in.shadow && !in.ventFan && std::fabs(e) <= LEARN_BAND_C);
      if (++learnS_ >= 30U) { learnS_ = 0U; learnPlant(); }
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
    if (std::isfinite(tau) && tau >= TAU_PRIOR * TAU_LO && tau <= TAU_PRIOR * TAU_HI) { tau_ = tau; tauTrusted_ = true; }   // the machine keeps a tau only when a climb measured it; the next clean climb measures it again
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
    float sum = 0.0f, uLo = 100.0f, uHi = 0.0f;
    for (uint16_t i = 0U; i < n; ++i) {
      sum += uAge(i) - ffAge(i) - hold_;                                      // the fan's share is spent on the fan, not on the air
      uLo = std::min(uLo, uAge(i)); uHi = std::max(uHi, uAge(i));
    }
    float lead = kAir_ * LEAD_GAIN * sum;
    // 2.9.2: when the command has been steady for the whole pipe (nothing inside it is changing) the heat on its way is what the air is ALREADY rising by, times the pipe
    // (LEAD_GAIN x PIPE_K x tau), and PV says how fast that is - whatever kAir and hold the model believes. A model that believes a gain 2.7 x too high, or a hold that a
    // frozen estimator has left 30 % too low (door latch), predicted 0.15 .. 1.5 degC of "heat on its way" under a PV that stood still: the command stayed at hold + 1.5 %
    // for 12 minutes (field log of 03/11) and at 19-21 % for 20 minutes after a door (rig). The measured slope caps that prediction.
    if (n >= static_cast<uint16_t>(PIPE_K * tau_) && uHi - uLo <= STEADY_PIPE_FRAC * std::max(1.0f, cap_) && lead > 0.0f) {
      lead = std::min(lead, LEAD_GAIN * PIPE_K * tau_ * std::max(0.0f, slope_ + 2.0f * lsSigma_));
    }
    // On a climb (far under SV) the measured slope is a floor for the heat in flight (a wrong kAir cannot hide it); near SV its noise must not count.
    if (std::isfinite(pv_) && sp_ - pv_ > 1.5f * farBand_) lead = std::max(lead, 0.8f * std::max(0.0f, slope_) * tau_);
    lead_ = std::max(-kAir_ * hold_ * tau_, std::min(1.2f * inflightMax_, lead));
  }
  // 2.9.2: the continuous learning of kAir and the transit time (PlantLearner above): a fit that passes its gates moves the numbers by LEARN_STEP of the way, every 30 s. Only
  // seconds within LEARN_BAND_C of SV are used: a climb has its own measurement, and the oven far from SV is not the oven that has to be held.
  void learnPlant() {
    float k = 0.0f, d = 0.0f, r2 = 0.0f;
    if (!learner_.fit(k, d, r2)) return;
    k = std::max(KAIR_PRIOR * KAIR_LO, std::min(KAIR_PRIOR * KAIR_HI, k));
    d = std::max(TAU_PRIOR * TAU_LO, std::min(TAU_PRIOR * TAU_HI, d));
    kAir_ += LEARN_STEP * (k - kAir_);
    tau_ += LEARN_STEP * (d - tau_);
    identified_ = tauMeasured_ = true;
  }
  // THE writer of hold: a disturbance observer. A model of PV is driven by the power that has reached the oven (the command one transit time ago);
  // the error between the measured and the model PV corrects the model, the load (hold) and the load's rate of change. Three poles at 1/(1.5 tau)
  // (a ramp of load - cold eggs, the walls giving back heat - is followed without a standing error), the rate leaks away over 8 tau so that it does not
  // wander when nothing supports it. The estimate is held while the door is open or PV is far from SV (the load is then not what the error is about).
  void observe() {
    const uint16_t delay = static_cast<uint16_t>(tau_ + 0.5f);
    if (histCount_ < static_cast<uint16_t>(delay + 2U) || !std::isfinite(pv_)) { pm_ = pv_; return; }
    // The scheduled fan (known, fed forward) and the door freeze the estimate. 2.9.2: the fan of the vent line (the oven is TOO HOT) does not: it masks a surplus
    // of heat, never a lack of it, so while it runs the estimate may only come DOWN. Frozen as before, the hold stood at its old, too high, level for the 10 minutes
    // of every run of the fan and PV stayed over SV exactly while the oven was cooling (rig, the room warming up: 29 % of the time over SV + 0.05).
    const bool oneSided = ventFan_ && !fanSched_;
    if (!std::isfinite(pm_) || door_ || doorLatch_ || fanSched_ || std::fabs(sp_ - pv_) > 2.0f * farBand_) { pm_ = pv_; rate_ = 0.0f; return; }
    const float u = uAge(delay);
    const float w = 1.0f / (OBS_POLE_TAU * tau_);
    const float err = pv_ - pm_;
    const float holdBefore = hold_;
    pm_ += kAir_ * (u - hold_) + 3.0f * w * err;
    const float rLim = cap_ / (2.0f * tau_);                      // the load cannot move faster than the whole ceiling in two transit times
    rate_ = (rate_ - (w * w * w / kAir_) * err) * (1.0f - 1.0f / (OBS_LEAK_TAU * tau_));
    rate_ = std::max(-rLim, std::min(rLim, rate_));
    hold_ = std::max(0.0f, std::min(cap_, hold_ + rate_ - (3.0f * w * w / kAir_) * err));
    if (oneSided && hold_ > holdBefore) { hold_ = holdBefore; rate_ = std::min(rate_, 0.0f); }
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
      tau_ = std::max(TAU_PRIOR * TAU_LO, std::min(TAU_PRIOR * TAU_HI, swT_ * SW_TAU_FACTOR)); tauMeasured_ = tauTrusted_ = true; swOn_ = false;
    }
  }
  PlantLearner learner_;
  uint16_t learnS_ = 0U;
  uint8_t state_ = StIdle;
  float pv_ = NAN, sp_ = 37.5f, cap_ = 100.0f;
  float kAir_ = KAIR_PRIOR, tau_ = TAU_PRIOR, hold_ = 12.0f;
  float epMax_ = 0.0f; bool epFresh_ = true; float latchT_ = 0.0f; bool doorLatch_ = false, nearSeen_ = false; float pm_ = NAN, rate_ = 0.0f;
  bool ventFan_ = false, fanSched_ = false, fanSchedPrev_ = false, fanRun_ = false;
  float fanPv0_ = 0.0f, fanAcc_ = 0.0f, fanT_ = 0.0f;
  float fanW_ = 0.0f;                       // what the scheduled fan takes out of the oven [% of the heater]: learned while it runs, fed forward one transit time ahead
  bool identified_ = false, tauMeasured_ = false, tauTrusted_ = false, door_ = false, wasLive_ = false;
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
