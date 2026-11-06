#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// ONE climb controller for every rise of the temperature (first climb, after a door, a set-point step, a restart), from any distance up to RELEASE_GAP_C under SV.
// Nothing but the distance PV-SV and the measured speed of the rise (the slope that comes from the timing of the 0.1 degC steps of the sensor):
//   - far away (the speed times the delay does not yet reach the distance): full power.
//   - near: the wanted speed is the distance divided by CLOSE_OF_TAU transit times (+60 s); every transit time the power is moved by the missing (or the excess) speed
//     divided by the gain of the oven (learned from what the last move did). Too slow: more power, unless the rise is still speeding up (what was given is still arriving).
//     Too fast: less power. No other brake.
//   - at RELEASE_GAP_C the stability controller takes over with the hold level = the power minus what the air still gains with it.
namespace thermalsimple {

#ifndef S_CLOSE_K
#define S_CLOSE_K 2.0f
#endif
#ifndef S_KPF
#define S_KPF 0.4f
#endif
#ifndef S_STEP
#define S_STEP 12.0f
#endif
constexpr float ARM_GAP_C = 0.25f;
constexpr float RELEASE_GAP_C = 0.10f;
#ifndef S_FAR
#define S_FAR 1.0f
#endif
constexpr float FAR_MIN_GAP_C = S_FAR;          // a flat PV nearer than this is a load, not a distance: full power is for the far climb only
constexpr float DEAD_RATE = 0.0002f;         // degC/s, under this the speed is "as wanted" (the sensor cannot tell)
constexpr float SLOPE_EMA_S = 45.0f;
constexpr float LAG_S = 45.0f;               // delay of the smoothed speed
constexpr float STEP_UP_PCT = S_STEP, STEP_DN_PCT = 2.0f * S_STEP;
constexpr float K_PRIOR_FACTOR = S_KPF;
constexpr float CLOSE_OF_TAU = S_CLOSE_K;
constexpr uint8_t RING = 32;                 // speeds, one per 10 s

struct Out { bool active = false; float powerPct = 0.0f; bool released = false; float releaseHold = 0.0f; };

class Climb {
 public:
  void reset() { *this = Climb(); }
  Out step(float pv, float sp, float slope, float cmdPct, float capPct, float tau, float kPrior, bool blocked, float dtS) {
    Out o;
    const float gap = sp - pv;
    if (std::isfinite(slope)) { if (!have_) { slopeF_ = slope; have_ = true; } else slopeF_ += (slope - slopeF_) * dtS / (SLOPE_EMA_S + dtS); }
    if (blocked || !std::isfinite(gap) || !have_) { if (active_) release(o); return o; }
    ringAcc_ += dtS;
    if (ringAcc_ >= 10.0f) { ringAcc_ -= 10.0f; ring_[head_] = slopeF_; head_ = static_cast<uint8_t>((head_ + 1U) % RING); if (n_ < RING) ++n_; }
    if (!active_) {
      if (gap < ARM_GAP_C) return o;
      active_ = true; P_ = std::max(0.0f, std::min(capPct, cmdPct)); k_ = std::max(1.0e-5f, K_PRIOR_FACTOR * kPrior); lastP_ = P_; lastR_ = slopeF_; tAcc_ = 0.0f;
    }
    if (gap <= RELEASE_GAP_C) { release(o); return o; }
    const float proj = std::max(0.0f, slopeF_) * (tau + LAG_S);
    if (gap > std::max(1.25f * proj + 0.3f, FAR_MIN_GAP_C)) {
      P_ = capPct; tAcc_ = 0.0f; lastP_ = P_; lastR_ = slopeF_;                  // far: full power
    } else {
      tAcc_ += dtS;
      if (tAcc_ >= tau) {
        tAcc_ = 0.0f;
        const float dP = P_ - lastP_, dR = slopeF_ - lastR_;
        if (std::fabs(dP) >= 2.0f) { const float kObs = dR / dP; if (kObs > 0.0f) k_ += 0.3f * (std::max(0.25f * k_, std::min(4.0f * k_, kObs)) - k_); }
        lastP_ = P_; lastR_ = slopeF_;
        const float want = gap / (CLOSE_OF_TAU * tau + 60.0f);
        const float err = want - slopeF_;
        const uint8_t back = static_cast<uint8_t>(std::min<uint8_t>(n_ - 1U, static_cast<uint8_t>(std::max(1.0f, tau / 10.0f))));
        const float rThen = n_ > back ? ring_[(head_ + RING - 1U - back) % RING] : slopeF_;
        const bool speedingUp = slopeF_ > rThen + DEAD_RATE;
        if (err > DEAD_RATE) { if (!speedingUp) P_ += std::min(STEP_UP_PCT, err / k_); }
        else if (err < -DEAD_RATE) P_ -= std::min(STEP_DN_PCT, -err / k_);
        P_ = std::max(0.0f, std::min(capPct, P_));
      }
    }
    o.active = true; o.powerPct = P_;
    return o;
  }
  bool active() const { return active_; }
 private:
  void release(Out &o) { o.released = true; o.releaseHold = std::max(0.0f, P_ - std::max(0.0f, slopeF_) / std::max(1.0e-5f, k_)); active_ = false; tAcc_ = 0.0f; }
  float ring_[RING] = {};
  uint8_t head_ = 0U, n_ = 0U;
  bool active_ = false, have_ = false;
  float P_ = 0.0f, k_ = 1.0e-4f, slopeF_ = 0.0f, lastP_ = 0.0f, lastR_ = 0.0f, tAcc_ = 0.0f, ringAcc_ = 0.0f;
};

}  // namespace thermalsimple
