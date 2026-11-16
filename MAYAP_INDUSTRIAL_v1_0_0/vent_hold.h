#pragma once
// The vent hold: when the profile / scheduled exhaust ventilation must wait. Pure C++11 (no Arduino types): MachineController asks it every cycle and the host
// test (tests/vent-hold.cpp) drives the very same code.
//
// The fan takes heat out exactly when the heater already works at its ceiling (a door, the recovery of a tray of cold eggs, the first climb), and the dip it adds
// is as deep as the run is long. So the ventilation of the day profile (and of the schedule) waits while the oven climbs, recovers from a door or stands far under SP,
// at the start of a batch and at any time of it. Forced vents (High, sensor fault, Emergency, the vent line) and a Web command never go through here.
//
//   HOLD    starts when PV is more than enterC under SP, or the controller sees a door; until the first usable sample of a batch decides, the batch start counts
//           as a hold (the first minutes of the sensor filter must not open the fan, for at most maxMs).
//   RELEASE PV is back to releaseC under SP (or nearer) and no door; or the sensor is not usable; or after maxMs. A weak heater must not keep the oven without
//           air for ever: after maxMs the rule waits for PV to come back to releaseC once before it can hold again.
//   A vent run that is skipped is skipped: the profile is a clock of the batch, nothing is caught up afterwards.
#include <cmath>
#include <cstdint>

namespace VentHoldRule {

struct Params {
  float enterC = 0.3f, releaseC = 0.2f;
  uint32_t maxMs = 1800000UL;
};

class Hold {
 public:
  bool holding() const { return hold_; }
  void reset() { hold_ = false; checked_ = false; expired_ = false; since_ = 0U; started_ = false; }
  // errC = SP - PV (positive: under SP). Returns whether the profile / scheduled vent must wait now.
  bool update(uint32_t now, bool batchRunning, bool sensorUsable, float errC, bool door, const Params &p) {
    if (!batchRunning) { reset(); return false; }
    if (!started_) { started_ = true; since_ = now; hold_ = false; checked_ = false; expired_ = false; }
    const bool err_ok = std::isfinite(errC);
    if (!checked_) {
      if (sensorUsable && err_ok) { checked_ = true; hold_ = errC > p.enterC || door; since_ = now; }
      else hold_ = elapsed(now, since_) < p.maxMs;
    } else if (hold_) {
      if (!sensorUsable || !err_ok || (errC <= p.releaseC && !door)) hold_ = false;
      else if (elapsed(now, since_) >= p.maxMs) { hold_ = false; expired_ = true; }
    } else if (sensorUsable && err_ok) {
      if (errC <= p.releaseC) expired_ = false;
      if (!expired_ && (errC > p.enterC || door)) { hold_ = true; since_ = now; }
    }
    return hold_;
  }

 private:
  static uint32_t elapsed(uint32_t now, uint32_t then) { return static_cast<uint32_t>(now - then); }
  bool hold_ = false, checked_ = false, expired_ = false, started_ = false;
  uint32_t since_ = 0U;
};

}  // namespace VentHoldRule
