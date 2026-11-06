#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// The ramp governor: owns the last stretch of a climb (PV under SV by RAMP_MIN_GAP_C .. RAMP_MAX_GAP_C). It predicts nothing from a model. It asks for a steady rise whose speed is the
// remaining gap divided by RAMP_CLOSE_S (so the speed falls as the gap closes), MEASURES how fast PV really rises (the slope that comes from the timing of the 0.1 degC steps of the sensor),
// and corrects its own power by the missing speed, in steps one transit time apart (the time the oven needs to show what a change did). The gain of the correction is the oven's own: after every
// step it compares the change of speed with the change of power it caused and learns degC/s per percent, so a slow oven gets big corrections and a fast one small ones, without a fixed number.
// If the rise stalls (a heavy load, cold eggs) the speed error stays positive and the power keeps being raised, every transit time, until PV moves; if the rise is too fast it is lowered.
// Hand-over: when the gap closes the governor gives the hold controller the power that holds the temperature (its power minus what the air is still gaining with it).
namespace thermalramp {

#ifndef R_CLOSE_V
#define R_CLOSE_V 600.0f
#endif
constexpr float RAMP_MAX_GAP_C = 1.2f;       // farther away: the hold law's own climb
constexpr float RAMP_MIN_GAP_C = 0.06f;      // closer: the hold law
constexpr float RAMP_ARM_GAP_C = 0.20f;      // it starts only when the gap is at least this
constexpr float RAMP_CLOSE_S = R_CLOSE_V;       // the gap should close in this time at the current speed
constexpr float RAMP_MAX_RATE = 0.004f;      // degC/s, never ask for more (0.24 degC/min)
#ifndef R_ALPHA
#define R_ALPHA 0.5f
#endif
#ifndef R_STEPMAX
#define R_STEPMAX 8.0f
#endif
#ifndef R_TS
#define R_TS 1.5f
#endif
#ifndef R_KPRIOR
#define R_KPRIOR 1.0f
#endif
#ifndef R_CLOSE
#define R_CLOSE 600.0f
#endif
constexpr float ALPHA = R_ALPHA;             // fraction of the missing power added per correction
constexpr float STEP_MAX_PCT = R_STEPMAX;
constexpr float TS_OF_TAU = R_TS, KPRIOR_FACTOR = R_KPRIOR;
constexpr float MIN_DP_PCT = 2.0f;           // learn the gain only from changes of at least this
constexpr float SLOPE_EMA_S = 60.0f;

struct Out { bool active = false; float powerPct = 0.0f; bool released = false; float releaseHold = 0.0f; float kEff = 0.0f; };

class Governor {
 public:
  void reset() { active_ = false; tAcc_ = 0.0f; }
  // pv/sp [degC], slope [degC/s] (NaN unknown), cmdPct = what the hold law asks for now, tau [s] = transit time, kPrior [degC/s per %], blocked = door, fan, protection, shadow.
  Out step(float pv, float sp, float slope, float cmdPct, float capPct, float tau, float kPrior, bool blocked, float dtS) {
    Out o;
    const float gap = sp - pv;
    if (std::isfinite(slope)) { if (!haveSlope_) { slopeF_ = slope; haveSlope_ = true; } else slopeF_ += (slope - slopeF_) * dtS / (SLOPE_EMA_S + dtS); }
    if (blocked || !std::isfinite(gap) || !std::isfinite(slopeF_)) { if (active_) release(o); return o; }
    if (!active_) {
      if (gap >= RAMP_ARM_GAP_C && gap < RAMP_MAX_GAP_C) {
        active_ = true; P_ = std::max(0.0f, std::min(capPct, cmdPct)); kEff_ = std::max(1.0e-5f, KPRIOR_FACTOR * kPrior);
        lastP_ = P_; lastR_ = slopeF_; tAcc_ = 0.0f;
      } else return o;
    }
    if (gap <= RAMP_MIN_GAP_C || gap >= 1.2f * RAMP_MAX_GAP_C) { release(o); return o; }
    tAcc_ += dtS;
    if (tAcc_ >= TS_OF_TAU * tau) {
      tAcc_ = 0.0f;
      const float want = std::min(RAMP_MAX_RATE, gap / RAMP_CLOSE_S);
      const float dP = P_ - lastP_, dR = slopeF_ - lastR_;
      if (std::fabs(dP) >= MIN_DP_PCT) {
        const float kObs = dR / dP;
        if (kObs > 0.0f) kEff_ += 0.3f * (std::max(0.25f * kEff_, std::min(4.0f * kEff_, kObs)) - kEff_);
      }
      lastP_ = P_; lastR_ = slopeF_;
      const float step = ALPHA * (want - slopeF_) / kEff_;
      P_ = std::max(0.0f, std::min(capPct, P_ + std::max(-STEP_MAX_PCT, std::min(STEP_MAX_PCT, step))));
    }
    o.active = true; o.powerPct = P_; o.kEff = kEff_;
    return o;
  }
  bool active() const { return active_; }
 private:
  void release(Out &o) {
    o.released = true;
    o.releaseHold = std::max(0.0f, P_ - std::max(0.0f, slopeF_) / std::max(1.0e-5f, kEff_));
    active_ = false; tAcc_ = 0.0f;
  }
  bool active_ = false, haveSlope_ = false;
  float P_ = 0.0f, kEff_ = 1.0e-4f, lastP_ = 0.0f, lastR_ = 0.0f, tAcc_ = 0.0f, slopeF_ = 0.0f;
};

}  // namespace thermalramp
