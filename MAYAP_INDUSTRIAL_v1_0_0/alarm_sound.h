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
//   The over-temperature lines (High, Emergency) are LOUD at once: the siren sounds the moment the line is crossed and stops when the reading is
//   0.2 degC under it. With the owner's lines (SV 37.5, High 37.7) that is: over 0.2 degC -> loud siren, back down 0.2 degC -> silence.
#include <cmath>
#include <cstdint>

namespace alarmsound {

class LoudVoice {
 public:
  struct Params {
    uint32_t steadyMs = 300000UL;         // the siren is continuous this long, then pulsed (a reading stuck over the line for hours must not be a wall of noise)
    uint32_t onMs = 5000UL, offMs = 25000UL;
    float offBelowC = 0.2f;               // sound ends when the reading is this far under the line ...
    uint32_t offConfirmMs = 2000UL;       // ... for this long (the lock of the heater has its own, longer, hysteresis)
    float rearmC = 0.0f;                  // after such an end it is loud again as soon as the reading is back at the line (+ this)
    float ackRiseC = 0.2f;                // ACK mutes; the siren is back when the reading rose this much since the ACK ...
    uint32_t ackCheckMs = 120000UL;       // ... or when, at every check of this length, it has not fallen by ackFallC ...
    float ackFallC = 0.1f;
    uint32_t ackMaxMs = 600000UL;         // ... and in any case after this long
  };

  void reset() { sounding_ = false; quiet_ = false; muted_ = false; offOn_ = false; }
  // Every cycle. `active` = the line itself (High: highTemperatureActive_; Emergency: emergencyActive_); `line` = its threshold.
  void update(uint32_t now, bool valid, float pv, float line, bool active, const Params &p) {
    if (!valid || !std::isfinite(pv) || !active) { reset(); return; }
    if (pv <= line - p.offBelowC) {
      if (!offOn_) { offOn_ = true; offSince_ = now; }
    } else {
      offOn_ = false;
    }
    if (offOn_ && static_cast<uint32_t>(now - offSince_) >= p.offConfirmMs) {      // fell 0.2 degC: silence now, whatever the lock of the heater still does
      quiet_ = true; sounding_ = false; muted_ = false; offOn_ = false;
      return;
    }
    if (quiet_) {
      if (pv >= line + p.rearmC) quiet_ = false; else return;
    }
    if (!sounding_) { sounding_ = true; muted_ = false; soundAt_ = now; return; }
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
  bool sounding_ = false, quiet_ = false, muted_ = false, offOn_ = false;
  uint32_t soundAt_ = 0U, offSince_ = 0U, muteStart_ = 0U, ackAt_ = 0U;
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
