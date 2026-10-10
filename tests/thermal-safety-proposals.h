#pragma once
// Candidate safety detectors for D2 (frozen / drifting sensor) and D3 (contactor latch). HARNESS-ONLY: nothing here is in the firmware
// and none of it may enter production before the design, state table and this evidence are approved.
//
// Every detector sees only what the firmware sees (filtered PV, the commanded SSR state, High/Emergency flags, time) and is run in
// OBSERVATION mode on every simulated run (it never changes the control), recording the first time it would have fired. That gives
// the false-positive count over the whole matrix (thousands of fault-free runs). In the sensor-path suite the same detectors can
// ACT (open the contactor) so latency and the plant-true peak are measured.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>

namespace sim {

constexpr int NUnexpl = 3, NRepHigh = 5, NFrozen = 2, NEnergy = 2;

// D3-a. Heat that nobody commanded: the SSR has been commanded OFF for a long time, yet PV above the set point keeps rising.
// (A hot room also does this; it is why PV must be above SP: below it the heater is simply not needed.)
struct UnexplParams { double offMinS, riseC, aboveSpC, holdS; };
class UnexplainedHeat {
 public:
  explicit UnexplainedHeat(UnexplParams p = {300, 0.3, 0.2, 60}) : p_(p) {}
  // called once per second
  bool update(double t, bool cmdOn, double pv, double sp) {
    if (cmdOn) lastOn_ = t;
    hist_.push_back(pv); if (hist_.size() > 121) hist_.pop_front();
    if (hist_.size() < 121 || !std::isfinite(pv)) { since_ = -1; return fired_; }
    const bool cond = (t - lastOn_) >= p_.offMinS && (pv - hist_.front()) >= p_.riseC && pv >= sp + p_.aboveSpC;
    if (!cond) since_ = -1; else { if (since_ < 0) since_ = t; if (t - since_ >= p_.holdS && !fired_) { fired_ = true; firedAt_ = t; } }
    return fired_;
  }
  double firedAt() const { return fired_ ? firedAt_ : -1.0; }
 private:
  UnexplParams p_; std::deque<double> hist_; double lastOn_ = -1e9, since_ = -1, firedAt_ = -1; bool fired_ = false;
};

// D3-b. High that comes back: N High rising edges within a window. `uncommandedOnly` counts an edge only if the mean commanded
// duty over the 300 s before it was below `maxDuty` (a thermal coast is preceded by heat; a stuck SSR is not).
class RepeatedHigh {
 public:
  RepeatedHigh(int n, double windowS, bool uncommandedOnly, double maxDuty = 0.15) : n_(n), w_(windowS), unc_(uncommandedOnly), maxDuty_(maxDuty) {}
  bool update(double t, bool cmdOn, bool highActive) {
    duty_.push_back(cmdOn ? 1 : 0); if (duty_.size() > 300) duty_.pop_front();
    if (highActive && !prevHigh_) {
      double m = 0; for (int v : duty_) m += v; m = duty_.empty() ? 0 : m / duty_.size();
      if (!unc_ || m < maxDuty_) { edges_.push_back(t); }
      while (!edges_.empty() && t - edges_.front() > w_) edges_.pop_front();
      if (static_cast<int>(edges_.size()) >= n_ && !fired_) { fired_ = true; firedAt_ = t; }
    }
    prevHigh_ = highActive;
    return fired_;
  }
  double firedAt() const { return fired_ ? firedAt_ : -1.0; }
 private:
  int n_; double w_; bool unc_; double maxDuty_;
  std::deque<int> duty_; std::deque<double> edges_; bool prevHigh_ = false, fired_ = false; double firedAt_ = -1;
};

// D2. A reading that does not move while the heater delivers more energy than the learned hold can explain.
// `holdFrac` < 0 means unknown (no qualified profile): the conservative assumption 0.5 is used, never less.
struct FrozenParams { double windowS, holdFactor, minOnS; };
class FrozenEnergy {
 public:
  explicit FrozenEnergy(FrozenParams p) : p_(p) {}
  bool update(double t, bool cmdOn, double reading, double holdFrac) {
    if (!std::isfinite(reading)) { ref_ = NAN; return fired_; }
    if (!std::isfinite(ref_) || std::fabs(reading - ref_) > 0.01) { ref_ = reading; t0_ = t; onS_ = 0; return fired_; }
    if (cmdOn) onS_ += 1.0;
    const double h = holdFrac >= 0 ? holdFrac : 0.5;
    if (t - t0_ >= p_.windowS && onS_ >= std::max(p_.minOnS, p_.holdFactor * h * (t - t0_)) && !fired_) { fired_ = true; firedAt_ = t; }
    return fired_;
  }
  double firedAt() const { return fired_ ? firedAt_ : -1.0; }
 private:
  FrozenParams p_; double ref_ = NAN, t0_ = 0, onS_ = 0, firedAt_ = -1; bool fired_ = false;
};

// D2/D3. Net heater energy without a temperature response: the ACTUAL on-time above what the learned hold explains would, with the learned
// gain, have raised PV by `needC`, and PV rose by less than a quarter of that. Unknown profile: the lowest plausible gain (0.005 degC/s) and
// hold 0.5 are used, so an unlearned oven is only judged on a long window. It is an energy-plausibility CHECK, not a protection: it cannot
// replace the independent thermostat (a drifting probe that follows a plant heated at hold duty is invisible to it).
struct EnergyParams { double windowS, needC; };
class NetEnergyNoRise {
 public:
  explicit NetEnergyNoRise(EnergyParams p) : p_(p) {}
  bool update(double t, bool cmdOn, double pv, double kh, double holdFrac) {
    on_.push_back(cmdOn ? 1 : 0); pv_.push_back(pv);
    const size_t w = static_cast<size_t>(p_.windowS);
    if (on_.size() > w + 1) { on_.pop_front(); pv_.pop_front(); }
    if (on_.size() < w + 1 || !std::isfinite(pv)) return fired_;
    double onS = 0; for (int v : on_) onS += v;
    const double h = holdFrac >= 0 ? holdFrac : 0.5, g = kh > 0 ? kh : 0.005;
    const double expected = g * (onS - h * p_.windowS);
    if (expected >= p_.needC && (pv - pv_.front()) < 0.25 * expected && !fired_) { fired_ = true; firedAt_ = t; }
    return fired_;
  }
  double firedAt() const { return fired_ ? firedAt_ : -1.0; }
 private:
  EnergyParams p_; std::deque<int> on_; std::deque<double> pv_; double firedAt_ = -1; bool fired_ = false;
};

struct ProposalSet {
  UnexplainedHeat unexpl[NUnexpl] = {UnexplainedHeat({180, 0.2, 0.0, 60}), UnexplainedHeat({300, 0.3, 0.2, 60}), UnexplainedHeat({600, 0.5, 0.4, 60})};
  RepeatedHigh rep[NRepHigh] = {RepeatedHigh(2, 3600, false), RepeatedHigh(3, 3600, false), RepeatedHigh(4, 3600, false), RepeatedHigh(2, 3600, true), RepeatedHigh(1, 3600, true)};
  FrozenEnergy frozen[NFrozen] = {FrozenEnergy({600, 1.3, 240}), FrozenEnergy({1200, 1.5, 360})};
  NetEnergyNoRise energy[NEnergy] = {NetEnergyNoRise({600, 3.0}), NetEnergyNoRise({1200, 3.0})};
  void update(double t, bool cmdOn, double pv, double sp, bool high, double reading, double holdFrac, double kh = -1) {
    for (auto &e : energy) e.update(t, cmdOn, pv, kh, holdFrac);
    for (auto &u : unexpl) u.update(t, cmdOn, pv, sp);
    for (auto &r : rep) r.update(t, cmdOn, high);
    for (auto &f : frozen) f.update(t, cmdOn, reading, holdFrac);
  }
};

}  // namespace sim
