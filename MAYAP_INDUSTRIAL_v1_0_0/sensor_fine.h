// Fine temperature estimate from a quantized sensor (0.1 degC steps -> ~0.01 degC effective), plus the slope.
//
// The module only reports multiples of `step` (0.1). A single reading is a staircase, but the PATTERN of readings is not: how long each
// step lasts tells the slope, and the slope tells where inside the step the temperature is. A least-squares line over the last
// `windowS` seconds of readings uses all of that at once: the value at "now" is the fine temperature, the line slope is the slope.
// The window shortens when the temperature moves fast (so the estimate follows) and lengthens when it is slow (so a few steps still
// fit in it and the slope stays smooth).
//
// This is an estimate for CONTROL (earlier brake, straight climb). Protections keep using the raw reading.
#pragma once
#include <cmath>
#include <cstdint>

namespace sensorfine {

struct Params {
  float minWindowS = 40.0f;   // shortest fit window (fast changes)
  float maxWindowS = 150.0f;  // longest fit window (flat / very slow)
  float stepsInWindow = 3.0f; // aim to have about this many quantization steps inside the window
  float maxSlope = 0.20f;     // degC/s clamp (12 degC/min)
};

constexpr int CAP = 160;  // samples kept (>= maxWindowS / sample period for periods >= 1 s)

class Estimator {
 public:
  void reset() { n_ = 0; head_ = 0; winS_ = 90.0f; x_ = NAN; v_ = 0.0f; }
  bool ready() const { return n_ >= 3; }
  float value() const { return x_; }            // fine temperature at the last sample
  float slopePerS() const { return v_; }
  float slopePerMin() const { return v_ * 60.0f; }
  float windowS() const { return winS_; }
  float valueAt(float ageS) const { return std::isfinite(x_) ? x_ + v_ * ageS : NAN; }

  // One decoded reading taken at tS (seconds, any origin, increasing).
  void update(float q, float tS, float step, const Params &p) {
    if (!std::isfinite(q)) return;
    if (n_ > 0) {
      const float last = t_[(head_ + CAP - 1) % CAP];
      if (!(tS > last) || tS - last > 30.0f) { n_ = 0; head_ = 0; }   // gap or clock restart: start over
    }
    t_[head_] = tS; q_[head_] = q; head_ = (head_ + 1) % CAP; if (n_ < CAP) ++n_;
    // use samples inside the window
    const float w = winS_;
    double sx = 0, sy = 0, sxx = 0, sxy = 0; int m = 0;
    for (int k = 0; k < n_; ++k) {
      const int i = (head_ + CAP - 1 - k) % CAP;
      const float x = t_[i] - tS;            // <= 0, relative to now
      if (-x > w) break;
      const double y = q_[i] - q;            // relative to the newest reading (numerical accuracy)
      sx += x; sy += y; sxx += static_cast<double>(x) * x; sxy += static_cast<double>(x) * y; ++m;
    }
    if (m < 3) { x_ = q; v_ = 0.0f; return; }
    const double den = m * sxx - sx * sx;
    double slope = 0.0, icpt = sy / m;
    if (den > 1e-6) { slope = (m * sxy - sx * sy) / den; icpt = (sy - slope * sx) / m; }
    if (slope > p.maxSlope) slope = p.maxSlope;
    if (slope < -p.maxSlope) slope = -p.maxSlope;
    x_ = static_cast<float>(q + icpt);       // value of the line at "now"
    v_ = static_cast<float>(slope);
    // next window: about `stepsInWindow` steps of movement fit inside it
    const float sp = std::fabs(v_);
    float nw = sp > 1e-6f ? p.stepsInWindow * step / sp : p.maxWindowS;
    if (nw < p.minWindowS) nw = p.minWindowS;
    if (nw > p.maxWindowS) nw = p.maxWindowS;
    winS_ += (nw - winS_) * 0.3f;
  }

 private:
  float t_[CAP] = {}, q_[CAP] = {};
  int n_ = 0, head_ = 0;
  float winS_ = 90.0f, x_ = NAN, v_ = 0.0f;
};

}  // namespace sensorfine
