#pragma once
// What makes a noise, and when it stops. Pure C++ (no Arduino types): MachineController feeds these every cycle and tests/alarm-sound.cpp drives the
// very same code.
//
//   Sound classes (decided per fault code in machine_control.h, faultSoundClass):
//     Siren  - the external siren and the buzzer: the two over-temperature lines (LoudVoice) and an output that does not obey
//     Alert  - the HMI buzzer until ACK or until the condition is gone: the faults that stop the heater (sensor, heater not heating, storage ...)
//     Chime  - three short beeps when it appears, a thin reminder while it lasts, no ACK: warnings that need someone, not now
//     Silent - shown on the HMI / Web / push only: diagnostics
//   Every sound follows the LIVE condition, never the stored record: a latched fault that is gone is silent at once (the record stays for ACK).
//
//   The over-temperature lines (High, Emergency) are not loud at once: crossing a line starts a grace time (the exhaust fan is forced on by the line and
//   normally brings the reading back, e.g. the overshoot of a first climb). The siren sounds only if the reading is STILL at or over the line when the
//   grace time ends, or at once when it is far over (panicC). It stops when the reading is clearly under the line (more than one sensor step, 10 s to confirm: 2.9.2), so the battery is not
//   spent on a siren that has nothing left to say, and a flicker at the line cannot restart the grace time. The protective actions (heater cut, forced fans) never wait for this: only the noise does.
#include <cmath>
#include <cstdint>

namespace alarmsound {

class LoudVoice {
 public:
  struct Params {
    uint32_t steadyMs = 300000UL;         // the siren is continuous this long, then pulsed (a reading stuck over the line for hours must not be a wall of noise)
    uint32_t onMs = 5000UL, offMs = 25000UL;
    uint32_t graceMs = 180000UL;          // the siren waits this long after the line is crossed (the fan has had its time to bring the reading down) ...
    // 2.9.2: panicC 0.5 -> 0.3; offBelowC 0 -> 0.15 and offConfirmMs 2 s -> 10 s. The reading is a 0.1 degC staircase that flickers between two steps at the line: a single
    // dip under it for 2 s forgot the crossing, the 180 s grace time started again, and a reading that stood over the line for an hour never sounded (field, 04/11).
    float panicC = 0.3f;                  // ... unless the reading is this far over the line: then it sounds at once
    float offBelowC = 0.15f;              // the crossing is forgotten (silence, new grace time) when the reading is this far under the line (more than one sensor step) ...
    uint32_t offConfirmMs = 10000UL;      // ... for this long (the lock of the heater has its own, longer, hysteresis)
    float ackRiseC = 0.2f;                // ACK mutes; the siren is back when the reading rose this much since the ACK ...
    uint32_t ackCheckMs = 120000UL;       // ... or when, at every check of this length, it has not fallen by ackFallC ...
    float ackFallC = 0.1f;
    uint32_t ackMaxMs = 600000UL;         // ... and in any case after this long
  };

  void reset() { sounding_ = false; armed_ = false; muted_ = false; offOn_ = false; }
  // Every cycle. `active` = the line itself (High: highTemperatureActive_; Emergency: emergencyActive_); `line` = its threshold.
  void update(uint32_t now, bool valid, float pv, float line, bool active, const Params &p) {
    if (!valid || !std::isfinite(pv) || !active) { reset(); return; }
    if (pv < line - p.offBelowC) {
      if (!offOn_) { offOn_ = true; offSince_ = now; }
    } else {
      offOn_ = false;
    }
    if (offOn_ && static_cast<uint32_t>(now - offSince_) >= p.offConfirmMs) {      // fell under the line: silence now, grace time starts again next crossing
      sounding_ = false; armed_ = false; muted_ = false; offOn_ = false;
      return;
    }
    if (!armed_) {
      if (pv < line) return;                                                         // not over the line yet
      armed_ = true; overSince_ = now;                                               // the grace time starts when the line is crossed
    }
    if (!sounding_) {
      if (pv >= line + p.panicC || static_cast<uint32_t>(now - overSince_) >= p.graceMs) { sounding_ = true; muted_ = false; soundAt_ = now; }
      return;
    }
    if (muted_) {
      if (pv >= ackPv_ + p.ackRiseC) {
        muted_ = false;
      } else if (static_cast<uint32_t>(now - muteStart_) >= p.ackMaxMs) {
        muted_ = false;
      } else if (static_cast<uint32_t>(now - ackAt_) >= p.ackCheckMs) {
        if (pv > ackPv_ - p.ackFallC) muted_ = false;                              // not falling: it speaks again
        else { ackPv_ = pv; ackAt_ = now; }                                        // falling: stay quiet, look again later
      }
    }
  }
  // The owner pressed ACK at the machine.
  void ack(uint32_t now, float pv) {
    if (!sounding_ || muted_) return;
    muted_ = true; muteStart_ = ackAt_ = now; ackPv_ = pv;
  }
  bool sounding() const { return sounding_; }
  bool audible() const { return sounding_ && !muted_; }
  bool muted() const { return muted_; }
  // The external siren at this moment (continuous at first, then pulsed).
  bool siren(uint32_t now, const Params &p) const {
    if (!audible()) return false;
    const uint32_t el = static_cast<uint32_t>(now - soundAt_);
    if (el < p.steadyMs) return true;
    const uint32_t period = p.onMs + p.offMs;
    return period == 0U || ((el - p.steadyMs) % period) < p.onMs;
  }

 private:
  bool sounding_ = false, armed_ = false, muted_ = false, offOn_ = false;
  uint32_t soundAt_ = 0U, overSince_ = 0U, offSince_ = 0U, muteStart_ = 0U, ackAt_ = 0U;
  float ackPv_ = 0.0f;
};

// The Chime class: a chime for every code that appears, a thin reminder while any of them lasts.
class Reminder {
 public:
  void reset() { prev_ = 0U; any_ = false; }
  // `live` = one bit per code that is in its condition now. Returns true when a chime is due.
  bool update(uint32_t now, uint64_t live, uint32_t remindMs) {
    bool due = false;
    if (live == 0U) {
      any_ = false;
    } else {
      if ((live & ~prev_) != 0U) due = true;                                          // a code that was not there before
      else if (!any_) due = true;
      else if (static_cast<uint32_t>(now - lastAt_) >= remindMs) due = true;
      any_ = true;
    }
    if (due) lastAt_ = now;
    prev_ = live;
    return due;
  }

 private:
  uint64_t prev_ = 0U;
  bool any_ = false;
  uint32_t lastAt_ = 0U;
};

}  // namespace alarmsound
