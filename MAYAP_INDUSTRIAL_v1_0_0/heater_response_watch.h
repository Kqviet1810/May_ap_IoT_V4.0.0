#pragma once
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
class HeaterResponseWatch {
 public:
  void reset() {
    tracking_ = false;
    active_ = false;
    sinceAt_ = 0U;
    accumOnMs_ = 0U;
    referenceC_ = NAN;
  }

  // `demand` = the machine is in a state where the heater is expected to raise the temperature (batch, heater enabled, sensor valid,
  // temperature below the set-point band). Without demand the evidence is dropped.
  void update(bool demand, float temperatureC, uint32_t now, bool ssrOn, float minRiseC, uint32_t requiredOnMs) {
    if (!demand || !std::isfinite(temperatureC)) {
      reset();
      sinceAt_ = now;
      return;
    }
    if (!tracking_) {
      tracking_ = true;
      sinceAt_ = now;
      accumOnMs_ = 0U;
      referenceC_ = temperatureC;
      active_ = false;
      return;
    }
    // A task that was late must not count as continuous heating: one control step is worth at most one second.
    const uint32_t gap = static_cast<uint32_t>(now - sinceAt_);
    const uint32_t dt = gap < 1000UL ? gap : 1000UL;
    sinceAt_ = now;
    if (ssrOn && accumOnMs_ < UINT32_MAX - dt) accumOnMs_ += dt;
    if (temperatureC < referenceC_) referenceC_ = temperatureC;       // follow the temperature down (see above)
    if (temperatureC - referenceC_ >= minRiseC) {
      // The oven responds: a new energy window starts from here.
      referenceC_ = temperatureC;
      accumOnMs_ = 0U;
      active_ = false;
    } else if (accumOnMs_ >= requiredOnMs) {
      active_ = true;
    }
  }

  bool active() const { return active_; }
  uint32_t accumOnMs() const { return accumOnMs_; }
  float referenceC() const { return referenceC_; }
  // How far the temperature is above the reference (the number the fault carries, in 0.1 degC).
  float riseC(float temperatureC) const {
    return (std::isfinite(referenceC_) && std::isfinite(temperatureC)) ? temperatureC - referenceC_ : 0.0f;
  }

 private:
  bool tracking_ = false;
  bool active_ = false;
  uint32_t sinceAt_ = 0U;
  uint32_t accumOnMs_ = 0U;
  float referenceC_ = NAN;
};
