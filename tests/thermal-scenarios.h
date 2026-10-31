#pragma once
// Scenarios that run the REAL Thermal V3 controller and the REAL E115 evidence class against a simulated oven (see thermal-sim.h).
// Host tests only.
//
// The same rig can also drive the controller and the E115 rule of firmware 2.3.4 (kept frozen in tests/fixtures/thermal-v3-2.3.4/): the
// 29/10 failure is reproduced on the old code and shown fixed on the new one on the SAME simulated oven.
#include "thermal-sim.h"
#ifndef THERMAL_V3_HEADER
#define THERMAL_V3_HEADER "../MAYAP_INDUSTRIAL_v1_0_0/thermal_v3.h"
#endif
#include THERMAL_V3_HEADER
#ifndef HEATER_WATCH_HEADER
#define HEATER_WATCH_HEADER "../MAYAP_INDUSTRIAL_v1_0_0/heater_response_watch.h"
#endif
#include HEATER_WATCH_HEADER
#ifdef THERMAL_SCENARIOS_WITH_LEGACY
#define thermalv3 thermalv3_v234
#include "fixtures/thermal-v3-2.3.4/thermal_v3.h"
#undef thermalv3
#endif

namespace thermalsim {

constexpr double SP_C = 37.5;
constexpr double BAND_C = 0.3;
constexpr double MIN_RISE_C = 0.3;               // config default heaterStuckMinRiseC
constexpr uint32_t E115_ON_MS = 900000UL;        // config default heaterStuckDurationSec
constexpr double HYST_C = 0.2;                   // config default tempHysteresis

// The E115 rule of firmware <= 2.3.4, as it was written in MachineController: the reference temperature is the one at the start of the
// energy window and it only ever moves UP (when the temperature has risen by minRiseC). Kept to show what it did to the 29/10 batch.
class LegacyHeaterWatch {
 public:
  void update(bool demand, float temperatureC, uint32_t now, bool ssrOn, float minRiseC, uint32_t requiredOnMs) {
    if (!demand) { tracking_ = false; since_ = now; accum_ = 0U; start_ = NAN; active_ = false; return; }
    if (!tracking_) { tracking_ = true; since_ = now; accum_ = 0U; start_ = temperatureC; active_ = false; return; }
    const uint32_t gap = static_cast<uint32_t>(now - since_);
    const uint32_t dt = gap < 1000UL ? gap : 1000UL;
    since_ = now;
    if (ssrOn) accum_ += dt;
    const float rise = std::isfinite(start_) ? temperatureC - start_ : 0.0f;
    if (std::isfinite(start_) && rise >= minRiseC) { start_ = temperatureC; accum_ = 0U; active_ = false; }
    else if (accum_ >= requiredOnMs) active_ = true;
  }
  bool active() const { return active_; }
  uint32_t accumOnMs() const { return accum_; }
 private:
  bool tracking_ = false, active_ = false;
  uint32_t since_ = 0U, accum_ = 0U;
  float start_ = NAN;
};

// One oven, one controller, one E115 watch, advancing in 0.1 s steps like the control task.
template <class Ctl, class In, class Watch>
class RigT {
 public:
  RigT(const PlantParams &p, double airC, double eggC, double sensorStepC = 0.1) : plant(p, airC, eggC), chain(sensorStepC) {
    for (int i = 0; i < 8; ++i) chain.sample(plant.sensor());
    in.sp = static_cast<float>(SP_C);
    in.dQ = static_cast<float>(sensorStepC);
    in.capPct = 100.0f;
    in.dtS = 0.1f;
  }
  // Seconds of simulated time with the door / the vent fan in the given state. Returns early when `stopWhen` says so (checked each second).
  template <class Stop>
  void advance(double seconds, bool door, bool vent, Stop stopWhen) {
    const long ticks = static_cast<long>(seconds * 10.0 + 0.5);
    for (long i = 0; i < ticks; ++i) {
      one(door, vent);
      if (tick_ % 10 == 0 && stopWhen(*this)) return;
    }
  }
  void advance(double seconds, bool door = false, bool vent = false) {
    advance(seconds, door, vent, [](const RigT &) { return false; });
  }

  double now() const { return t_; }
  double lastU() const { return lastU_; }
  bool tripped() const { return tripped_; }
  double tripTime() const { return tripT_; }
  float setpoint() const { return in.sp; }
  void setSetpoint(double sp) { in.sp = static_cast<float>(sp); }
  // A reboot (an update, a power blip): everything the controller and the E115 rule had learned or counted is gone.
  void restartControllers() { ctl = Ctl(); watch = Watch(); }
  // The controller runs from power-up but the batch starts later: until `seconds` the permit is off (the controller pushes zeros into its
  // ring and the heater is not asked for anything), which is how every first start of a power cycle reaches the controller on the machine.
  void startAt(double seconds) { startS_ = seconds; }
  // 1 Hz traces of the TRUE air temperature, the value the controller saw, and the command.
  const std::vector<float> &air() const { return ta_; }
  const std::vector<float> &seen() const { return pv_; }
  const std::vector<float> &cmd() const { return u_; }
  double sampleTime(size_t i) const { return t0_ + static_cast<double>(i + 1); }

  Plant plant;
  SensorChain chain;
  Ctl ctl;
  Watch watch;
  In in;

 private:
  void one(bool door, bool vent) {
    if (tick_ % 20 == 0) chain.sample(plant.sensor());
    in.pv = chain.value();
    in.permit = !tripped_ && t_ >= startS_;
    in.ventFan = vent;
    const auto o = ctl.step(in);
    const double u = tripped_ ? 0.0 : o.powerPct;
    lastU_ = u;
    plant.step(u, vent, door);
    duty_ += u / 100.0;
    bool on = false;
    if (duty_ >= 1.0) { on = true; duty_ -= 1.0; }
    ++tick_;
    t_ = static_cast<double>(tick_) * 0.1;
    if (!tripped_) {
      const bool demand = in.pv < in.sp - static_cast<float>(HYST_C);
      watch.update(demand, in.pv, static_cast<uint32_t>(t_ * 1000.0 + 0.5), on, static_cast<float>(MIN_RISE_C), E115_ON_MS);
      if (watch.active()) { tripped_ = true; tripT_ = t_; }
    }
    if (tick_ % 10 == 0) { ta_.push_back(static_cast<float>(plant.air())); pv_.push_back(in.pv); u_.push_back(static_cast<float>(u)); }
  }
  long tick_ = 0;
  double t_ = 0.0, t0_ = 0.0, duty_ = 0.0, lastU_ = 0.0, tripT_ = -1.0, startS_ = 0.0;
  bool tripped_ = false;
  std::vector<float> ta_, pv_, u_;
};

using Rig = RigT<thermalv3::Controller, thermalv3::Input, HeaterResponseWatch>;
#ifdef THERMAL_SCENARIOS_WITH_LEGACY
using LegacyRig = RigT<thermalv3_v234::Controller, thermalv3_v234::Input, LegacyHeaterWatch>;
#endif

// Time for the air to reach SP - BAND with the heater at 100 % from the same start and no control: the physical limit of that oven.
inline double fastestReach(const PlantParams &p, double airC, double eggC, double spC = SP_C) {
  Plant pl(p, airC, eggC);
  for (long i = 0; i < 20L * 3600L * 10L; ++i) {
    pl.step(100.0, false, false);
    if (pl.air() >= spC - BAND_C) return static_cast<double>(i + 1) * 0.1;
  }
  return -1.0;
}

struct Outcome {
  double reachS = -1;       // first time the air is within BAND of SP, from the start of the measured part
  double overC = 0;         // highest air temperature above SP after the reach
  double settleS = -1;      // after this time the air stays within BAND of SP until the end (from the start of the measured part)
  double minC = 1e9;        // lowest air temperature of the measured part
  double finalBandC = 0;    // largest |air - SP| over the last 30 minutes
  double finalMeanC = 0;    // mean (air - SP) over the last 30 minutes: a steady offset the band test would hide
  double finalPpC = 0;      // peak-to-peak air temperature over the last 30 minutes
  double belowS = 0;        // seconds of the last hour spent more than BAND under SP
  double tripS = -1;        // E115 trip time (from the start of the measured part), -1 = none
  double meanU = 0;         // mean command over the measured part
  double minFarPct = 101.0; // lowest command while the air was more than 2.5 degC under SP (101 = it never was); set by the scenarios that look
};

// Evaluate the 1 Hz trace from index `from`. `sp` may differ from SP_C (a set-point step).
template <class R>
inline Outcome evaluate(const R &r, size_t from, double sp = SP_C) {
  Outcome o;
  const std::vector<float> &ta = r.air();
  const size_t n = ta.size();
  if (n <= from) return o;
  double sumU = 0;
  for (size_t i = from; i < n; ++i) {
    const double t = static_cast<double>(i - from + 1);
    const double v = ta[i];
    o.minC = std::min(o.minC, v);
    sumU += r.cmd()[i];
    if (o.reachS < 0 && v >= sp - BAND_C) o.reachS = t;
    if (o.reachS >= 0) o.overC = std::max(o.overC, v - sp);
  }
  o.meanU = sumU / static_cast<double>(n - from);
  // settle: the last time the air was outside the band decides
  long lastOut = -1;
  for (size_t i = from; i < n; ++i) if (std::fabs(ta[i] - sp) > BAND_C) lastOut = static_cast<long>(i - from + 1);
  o.settleS = lastOut < 0 ? 0.0 : (static_cast<size_t>(lastOut) >= n - from ? -1.0 : static_cast<double>(lastOut));
  const size_t tailFrom = n > from + 1800 ? n - 1800 : from;
  double tailMin = 1e9, tailMax = -1e9, tailSum = 0;
  for (size_t i = tailFrom; i < n; ++i) {
    o.finalBandC = std::max(o.finalBandC, std::fabs(ta[i] - sp));
    tailMin = std::min(tailMin, static_cast<double>(ta[i]));
    tailMax = std::max(tailMax, static_cast<double>(ta[i]));
    tailSum += ta[i] - sp;
  }
  if (n > tailFrom) { o.finalMeanC = tailSum / static_cast<double>(n - tailFrom); o.finalPpC = tailMax - tailMin; }
  const size_t hourFrom = n > from + 3600 ? n - 3600 : from;
  for (size_t i = hourFrom; i < n; ++i) if (ta[i] < sp - BAND_C) o.belowS += 1.0;
  if (r.tripped()) {
    const double startT = r.sampleTime(from) - 1.0;
    o.tripS = r.tripTime() - startT;
  }
  return o;
}

}  // namespace thermalsim
