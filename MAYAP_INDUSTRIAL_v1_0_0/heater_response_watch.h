#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// E115 "heater not heating" evidence. Pure C++ (no Arduino types): MachineController calls it every control cycle and the host tests
// drive the very same code (tests/thermal-e115.cpp with the real output arbiter, tests/thermal-closed-loop.cpp on whole ovens).
//
// The rule: while the machine wants heat, add up the time the SSR really conducts; if that reaches `requiredOnMs` (the configured
// full-power on-time) without the temperature having risen by `minRiseC` above the REFERENCE, the heater is not heating.
//
// The reference is the LOWEST temperature seen since the window began. It used to be the temperature at the start of the window and it
// never moved down: after a door or a tray of cold eggs pulled the oven 3 degC under that starting value, a perfectly healthy heater
// that was lifting the temperature again (33.3 -> 35.0 degC over 20 minutes) never "rose" above the stale reference, the on-time piled
// up and the machine cut the heater at the exact moment the oven needed it most. With the reference following the temperature
// down, only a temperature that does NOT recover while the heater has delivered `requiredOnMs` of full-power energy counts as evidence.
//
// Both the reference and the rise are taken on the temperature SMOOTHED with a time constant of one minute. The lowest value of a raw,
// noisy signal keeps falling with every wobble (the range of N samples grows with N) and the next wobble then looks like a rise: with a
// sensor that wanders by a few tenths of a degree the window was reset again and again and a dead heater was never found. A minute of
// smoothing costs a healthy heater nothing (its rise is detected a minute later at most) and finds a dead one about as fast as before.
class HeaterResponseWatch {
 public:
  static constexpr double SMOOTH_S = 60.0;

  void reset() {
    tracking_ = false;
    active_ = false;
    sinceAt_ = 0U;
    accumOnMs_ = 0U;
    pausedMs_ = 0U;
    referenceC_ = NAN;
    smoothC_ = NAN;
  }

  // `demand` = the machine is in a state where the heater is expected to raise the temperature (batch, heater enabled, sensor valid,
  // temperature below the set-point band). Without demand the evidence is dropped.
  //
  // `pauseEvidence`: the machine suspects an open door. While the temperature falls or stands still because cold air pours in, a healthy heater looks exactly like
  // a dead one (nothing in the temperature tells them apart), so the on-time is not added while the door is suspected. The pause is bounded: after
  // `requiredOnMs` of pause the evidence counts again, so a heater that is really dead is still found, at most one window later.
  void update(bool demand, float temperatureC, uint32_t now, bool ssrOn, float minRiseC, uint32_t requiredOnMs, bool pauseEvidence = false) {
    if (!demand || !std::isfinite(temperatureC)) {
      reset();
      sinceAt_ = now;
      return;
    }
    if (!tracking_) {
      tracking_ = true;
      sinceAt_ = now;
      accumOnMs_ = 0U;
      pausedMs_ = 0U;
      smoothC_ = temperatureC;
      referenceC_ = temperatureC;
      active_ = false;
      return;
    }
    // A task that was late must not count as continuous heating: one control step is worth at most one second.
    const uint32_t gap = static_cast<uint32_t>(now - sinceAt_);
    const uint32_t dt = gap < 1000UL ? gap : 1000UL;
    sinceAt_ = now;
    if (pauseEvidence && pausedMs_ < requiredOnMs) {
      if (ssrOn) pausedMs_ += dt;                                         // the door: the on-time of this second is not evidence, only counted aside
    } else if (ssrOn && accumOnMs_ < UINT32_MAX - dt) {
      accumOnMs_ += dt;
    }
    const double dtS = static_cast<double>(dt) * 0.001;
    smoothC_ += (static_cast<double>(temperatureC) - smoothC_) * (dtS / (SMOOTH_S + dtS));
    if (smoothC_ < referenceC_) referenceC_ = smoothC_;                   // follow the temperature down (see above)
    if (smoothC_ - referenceC_ >= static_cast<double>(minRiseC)) {
      // The oven responds: a new energy window starts from here.
      referenceC_ = smoothC_;
      accumOnMs_ = 0U;
      pausedMs_ = 0U;
      active_ = false;
    } else if (accumOnMs_ >= requiredOnMs) {
      active_ = true;
    }
  }

  bool active() const { return active_; }
  uint32_t accumOnMs() const { return accumOnMs_; }
  uint32_t pausedMs() const { return pausedMs_; }
  float referenceC() const { return static_cast<float>(referenceC_); }
  // How far the temperature is above the reference (the number the fault carries, in 0.1 degC).
  float riseC(float temperatureC) const {
    return (std::isfinite(referenceC_) && std::isfinite(temperatureC)) ? static_cast<float>(static_cast<double>(temperatureC) - referenceC_) : 0.0f;
  }

 private:
  bool tracking_ = false;
  bool active_ = false;
  uint32_t sinceAt_ = 0U;
  uint32_t accumOnMs_ = 0U;
  uint32_t pausedMs_ = 0U;
  double referenceC_ = NAN;
  double smoothC_ = NAN;
};

// E115 "heater not heating" - the rule that raises the alarm (3.0.1). It is deliberately simple: the heater has been asked for (nearly) everything for two hours and the
// temperature still FALLS. An oven whose heater works never does that: with the power at its ceiling the air stands still or rises, however heavy the load (cold eggs, a tray, a
// cold room); only a heater that gives nothing, or an oven that cannot hold the set-point at all, lets it fall, and then the owner has to know.
// The first rule (HeaterResponseWatch above: 900 s of SSR on-time without a rise of 0.3 degC) could not tell a heavy load from a dead heater: the heavy loads of a real batch
// (a tray of cold eggs, a cold day, a weak heater) kept the rise under 0.3 degC for longer than 900 s of on-time in 5 of 21 simulated healthy ovens and stopped them.
// It is still kept: its evidence counter feeds the door warning, the frozen-sensor check and the controller's push of the hold; only the alarm comes from here.
//   - high power: the request is at least HIGH_FRACTION of the ceiling (a dead heater makes PV fall at once, so the controller's request goes to the ceiling within minutes);
//     a gap of up to GAP_S under that does not break the stretch, a longer one does;
//   - strong fall: the stretch has lasted WINDOW_S and the one-minute smoothed temperature is FALL_C under its highest value of the stretch.
// A dead heater in a very well insulated oven falls slower than that (0.4 degC an hour in the slowest oven of the family at a room of 30 degC): the low-temperature alarm catches it.
class HeaterFallWatch {
 public:
  static constexpr double SMOOTH_S = 60.0, WINDOW_S = 7200.0, GAP_S = 120.0;
  static constexpr float HIGH_FRACTION = 0.85f, FALL_C = 1.0f;

  void reset() {
    tracking_ = false;
    active_ = false;
    sinceAt_ = 0U;
    smoothC_ = NAN;
    highS_ = lowS_ = 0.0;
    peakC_ = -1.0e9;
  }

  // `demand` as for HeaterResponseWatch. `requestPct`: what the controller asks of the heater now; `capPct`: the ceiling it may ask (the owner's power limit).
  void update(bool demand, float temperatureC, uint32_t now, float requestPct, float capPct) {
    if (!demand || !std::isfinite(temperatureC)) {
      reset();
      sinceAt_ = now;
      return;
    }
    if (!tracking_) {
      tracking_ = true;
      sinceAt_ = now;
      smoothC_ = temperatureC;
      return;
    }
    const uint32_t gap = static_cast<uint32_t>(now - sinceAt_);
    const uint32_t dt = gap < 1000UL ? gap : 1000UL;
    sinceAt_ = now;
    const double dtS = static_cast<double>(dt) * 0.001;
    smoothC_ += (static_cast<double>(temperatureC) - smoothC_) * (dtS / (SMOOTH_S + dtS));
    const bool high = std::isfinite(requestPct) && capPct > 0.0f && requestPct >= HIGH_FRACTION * capPct;
    if (high) {
      lowS_ = 0.0;
      if (highS_ <= 0.0) peakC_ = smoothC_;                               // a new stretch: its highest value so far is the present one
      highS_ += dtS;
      peakC_ = std::max(peakC_, smoothC_);
    } else {
      lowS_ += dtS;
      if (lowS_ > GAP_S) { highS_ = 0.0; peakC_ = -1.0e9; }
    }
    active_ = highS_ >= WINDOW_S && peakC_ - smoothC_ >= static_cast<double>(FALL_C);
  }

  bool active() const { return active_; }
  double highSeconds() const { return highS_; }
  float fallC() const { return highS_ > 0.0 && peakC_ > -1.0e8 ? static_cast<float>(std::max(0.0, peakC_ - smoothC_)) : 0.0f; }

 private:
  bool tracking_ = false;
  bool active_ = false;
  uint32_t sinceAt_ = 0U;
  double smoothC_ = NAN;
  double highS_ = 0.0, lowS_ = 0.0, peakC_ = -1.0e9;
};
