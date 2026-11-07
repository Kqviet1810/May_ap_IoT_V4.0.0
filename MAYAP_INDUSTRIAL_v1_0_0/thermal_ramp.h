#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// The ramp governor: the last stretch of a climb (PV under SV by RAMP_ARM_GAP_C .. RAMP_MAX_GAP_C). Nothing in it is a fixed number of the oven.
//
//  1. MEASURE. The rise of PV is the slope that comes from the timing of the 0.1 degC steps of the sensor (smoothed over a minute). Every 10 s it stores a pair: the rise of that moment and the power
//     that was given one transit time (+ the lag of the smoothing) earlier - the power that is now showing in the rise.
//  2. IDENTIFY, continuously. Through the last 10 minutes of pairs it fits   rise = k * (power - L)   (least squares, a prior of the oven's old numbers only as a handful of pseudo points that the data
//     outweigh within minutes). k = degC/s per percent the oven really has TODAY (cold eggs, a weak element, a sunny noon), L = the power that holds the temperature (the load).
//  3. COMMAND. It asks the oven for a rise that closes the PREDICTED gap in RAMP_CLOSE_S. Predicted = the gap minus what the power already on its way will still add over one transit time:
//          gap_pred = gap - k * (power_now - L) * transit
//     and the power that gives that rise is     power = L + (gap_pred / RAMP_CLOSE_S) / k .
//     The gap closes like an exponential (never faster than it can be braked), and when the heat already on its way is enough, gap_pred reaches zero: the power falls to L, and under zero BELOW L: that is the brake.
//     The command moves at most RAMP_SLEW_PCT per update (20 s), up and down.
//  4. HAND OVER. At the end (gap under RAMP_MIN_GAP_C, or PV past SV) the hold controller gets L as its hold level, with a power history that says the oven has been held at L.
// A stall (a heavy load) shows up as a small k*(power - L) in the fit: the identified L rises to the power that is being given, the command follows L, and the climb continues - all from what is measured.
namespace thermalramp {

#ifndef R_CLOSE_K
#define R_CLOSE_K 2.0f
#endif
#ifndef R_SLEW
#define R_SLEW 6.0f
#endif
#ifndef R_KPF
#define R_KPF 0.4f
#endif
#ifndef R_PW
#define R_PW 8.0f
#endif
#ifndef R_MAXGAP
#define R_MAXGAP 0.8f
#endif
constexpr float RAMP_MAX_GAP_C = R_MAXGAP;   // farther away: the hold law's own climb
constexpr float RAMP_MIN_GAP_C = 0.06f;      // closer: the hold law
constexpr float RAMP_ARM_GAP_C = 0.20f;      // it starts only when the gap is at least this
constexpr float CLOSE_OF_TAU = R_CLOSE_K;    // the predicted gap closes in this many transit times (+ 60 s)
constexpr float RAMP_SLEW_PCT = R_SLEW;
constexpr float RAMP_MAX_RATE = 0.004f;      // degC/s, never ask for more
constexpr float SLOPE_EMA_S = 60.0f;
constexpr float REG_EXTRA_DELAY_S = 70.0f;   // the smoothing of the slope and the window of the sensor estimate delay the rise by about this much
constexpr uint16_t HIST = 900;               // seconds of the power given
constexpr uint8_t NPAIR = 60;                // pairs of (power, rise), one per 10 s: ten minutes
constexpr float UPDATE_S = 20.0f;
constexpr float PRIOR_PSEUDO_W = R_PW;
constexpr float K_PRIOR_FACTOR = R_KPF;     // the old gain is trusted at this fraction: an oven under load or with cold eggs is always slower than its priors, never faster by much

struct Out { bool active = false; float powerPct = 0.0f; bool released = false; float releaseHold = 0.0f; float k = 0.0f; float L = 0.0f; };

class Governor {
 public:
  void reset() { *this = Governor(); }
  // pv/sp [degC], slope [degC/s] (NaN unknown), cmdPct = what the hold law asks for, holdOld = its hold estimate [%], tau = transit time [s], kPrior = its gain [degC/s per %].
  Out step(float pv, float sp, float slope, float cmdPct, float holdOld, float capPct, float tau, float kPrior, bool blocked, float dtS) {
    Out o;
    const float gap = sp - pv;
    if (std::isfinite(slope)) { if (!haveSlope_) { slopeF_ = slope; haveSlope_ = true; } else slopeF_ += (slope - slopeF_) * dtS / (SLOPE_EMA_S + dtS); }
    secAcc_ += dtS;
    bool tick = false;
    if (secAcc_ >= 1.0f) { secAcc_ -= 1.0f; tick = true; }
    if (blocked || !std::isfinite(gap) || !haveSlope_) {
      if (active_) release(o);
      if (tick) pushU(cmdPct);
      pairAcc_ = 0.0f; nPair_ = 0U; pairHead_ = 0U;                     // a disturbance: what was measured before it does not describe the oven now
      return o;
    }
    if (tick) { pushU(active_ ? P_ : cmdPct); pairAcc_ += 1.0f; updAcc_ += 1.0f; }
    const uint16_t lag = static_cast<uint16_t>(std::min<float>(HIST - 2U, tau + REG_EXTRA_DELAY_S));
    if (tick && pairAcc_ >= 10.0f && uCount_ > lag + 1U) { pairAcc_ = 0.0f; storePair(uAge(lag) / 100.0f, slopeF_ * 1000.0f); }
    if (!active_) {
      if (gap >= RAMP_ARM_GAP_C && gap < RAMP_MAX_GAP_C && nPair_ >= 6U) {
        active_ = true; P_ = std::max(0.0f, std::min(capPct, cmdPct)); updAcc_ = UPDATE_S; k_ = std::max(1.0e-5f, K_PRIOR_FACTOR * kPrior); L_ = std::max(0.0f, holdOld);
      } else return o;
    }
    if (gap <= RAMP_MIN_GAP_C || gap >= 1.2f * RAMP_MAX_GAP_C) { release(o); return o; }
    if (tick && updAcc_ >= UPDATE_S) {
      updAcc_ = 0.0f;
      identify(kPrior, holdOld, capPct);
      const float rFuture = k_ * (P_ - L_);                                  // the rise the power already given will show
      const float gapPred = gap - rFuture * tau;
      const float closeS = CLOSE_OF_TAU * tau + 60.0f;
      float rTarget = gapPred / closeS;
      rTarget = std::max(-RAMP_MAX_RATE, std::min(RAMP_MAX_RATE, rTarget));
      const float want = std::max(0.0f, std::min(capPct, L_ + rTarget / k_));
      P_ += std::max(-RAMP_SLEW_PCT, std::min(RAMP_SLEW_PCT, want - P_));
    }
    o.active = true; o.powerPct = P_; o.k = k_; o.L = L_;
    return o;
  }
  bool active() const { return active_; }
 private:
  float uAge(uint16_t a) const { return u_[(uHead_ + HIST - 1U - a) % HIST]; }
  void pushU(float v) { u_[uHead_] = v; uHead_ = static_cast<uint16_t>((uHead_ + 1U) % HIST); if (uCount_ < HIST) ++uCount_; }
  void storePair(float x, float y) { px_[pairHead_] = x; py_[pairHead_] = y; pairHead_ = static_cast<uint8_t>((pairHead_ + 1U) % NPAIR); if (nPair_ < NPAIR) ++nPair_; }
  // rise*1000 = a * x + b, x = power/100: least squares over the pairs plus two pseudo pairs of the old numbers (the line through the old L with the old k).
  void identify(float kPrior, float holdOld, float capPct) {
    if (nPair_ < 6U) return;
    const double a0 = 1.0e5 * static_cast<double>(std::max(1.0e-6f, K_PRIOR_FACTOR * kPrior));
    const double x0 = static_cast<double>(std::max(0.0f, holdOld)) / 100.0;
    double sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
    auto add = [&](double x, double y, double w) { sw += w; sx += w * x; sy += w * y; sxx += w * x * x; sxy += w * x * y; };
    for (uint8_t i = 0U; i < nPair_; ++i) add(px_[i], py_[i], 1.0);
    add(x0 - 0.1, a0 * (-0.1), PRIOR_PSEUDO_W); add(x0 + 0.1, a0 * 0.1, PRIOR_PSEUDO_W);
    const double det = sw * sxx - sx * sx;
    if (det < 1.0e-9) return;
    double a = (sw * sxy - sx * sy) / det, b = (sy - a * sx) / sw;
    a = std::max(0.15 * a0, std::min(4.0 * a0, a));
    b = (sy - a * sx) / sw;                                                // intercept for the clamped slope
    const double L = -b / a * 100.0;                                       // y = 0 at x = -b/a
    k_ = static_cast<float>(a / 1.0e5);
    L_ = static_cast<float>(std::max(0.0, std::min(static_cast<double>(capPct), L)));
  }
  void release(Out &o) {
    o.released = true;
    o.releaseHold = std::max(0.0f, L_);
    active_ = false; updAcc_ = 0.0f;
  }
  float u_[HIST] = {};
  float px_[NPAIR] = {}, py_[NPAIR] = {};
  uint16_t uHead_ = 0U, uCount_ = 0U;
  uint8_t pairHead_ = 0U, nPair_ = 0U;
  bool active_ = false, haveSlope_ = false;
  float P_ = 0.0f, k_ = 1.0e-4f, L_ = 0.0f, slopeF_ = 0.0f, secAcc_ = 0.0f, pairAcc_ = 0.0f, updAcc_ = 0.0f;
};

}  // namespace thermalramp
