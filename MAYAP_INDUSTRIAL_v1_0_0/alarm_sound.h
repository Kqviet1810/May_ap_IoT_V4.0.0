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
//   The over-temperature lines (High, Emergency) have ONE rule: the siren sounds as long as the alarm is on and is silent the moment it is off. The
//   alarm itself is a plain comparison of the temperature with its line (machine_control.h, updateAlarms): on when the reading is at/over the line, off
//   when the filtered reading is under it for a few seconds. Nothing waits, nothing is latched, nothing is reset by a dip (versions before 2.9.5 waited
//   180 s and started the wait again at every dip: the siren never sounded on a reading that flickered around the line).
//   A continuous siren for five minutes, then 5 s on / 25 s off, so that a reading stuck over the line for hours does not drain the battery.
//   2.9.9: the High siren goes through a SirenGate (time over the line is accumulated, a reading well over sounds at once, a dip of a few seconds does not start it all over) - the
//   plain comparison above flipped on and off at every sensor step of a reading that stood on the line and each time began again with the continuous siren.
#include <cmath>
#include <cstdint>

namespace alarmsound {

// The gate between the High alarm (E111, a plain comparison that flips every time the reading touches the line and leaves it again: the sensor steps 0.1 degC) and the siren.
// Time over the line is ACCUMULATED (a second over adds a second, a second under takes a quarter of one off), so a reading that flickers around the line adds up and the siren
// starts ONCE after dwellMs; a reading that is clearly over (immediateC above the line) sounds at once. Once on, it stays on until the alarm has been off for releaseMs.
// The protective actions of the alarm (heater, contactor, fans) do not go through this gate. Rig / log of 05/11: a High line of 37.7 with the reading stuck at the vent line sounded,
// stopped and sounded again every few seconds, each time from the continuous siren.
class SirenGate {
 public:
  struct Params {
    uint32_t dwellMs = 60000UL;
    float immediateC = 0.1f;
    uint32_t releaseMs = 30000UL;
  };
  void reset() { accumMs_ = quietMs_ = 0U; on_ = false; last_ = 0U; haveLast_ = false; }
  bool update(uint32_t now, bool valid, float pv, float line, bool active, const Params &p) {
    if (!valid || !std::isfinite(pv) || !std::isfinite(line)) { reset(); return false; }
    uint32_t dt = haveLast_ ? static_cast<uint32_t>(now - last_) : 0U;
    if (dt > 2000U) dt = 2000U;                                          // a missed cycle counts for what a cycle is
    last_ = now; haveLast_ = true;
    if (active) {
      accumMs_ += dt; quietMs_ = 0U;
      if (pv >= line + p.immediateC) on_ = true;
    } else {
      quietMs_ += dt;
      accumMs_ = accumMs_ > dt / 4U ? accumMs_ - dt / 4U : 0U;
    }
    if (!on_ && accumMs_ >= p.dwellMs) on_ = true;
    if (on_ && !active && quietMs_ >= p.releaseMs) { on_ = false; accumMs_ = 0U; }
    return on_;
  }
  bool on() const { return on_; }

 private:
  uint32_t accumMs_ = 0U, quietMs_ = 0U, last_ = 0U;
  bool on_ = false, haveLast_ = false;
};

class LoudVoice {
 public:
  struct Params {
    uint32_t steadyMs = 300000UL;         // the siren is continuous this long, then pulsed
    uint32_t onMs = 5000UL, offMs = 25000UL;
    float ackRiseC = 0.2f;                // ACK mutes; the siren is back when the reading rose this much since the ACK ...
    uint32_t ackCheckMs = 120000UL;       // ... or when, at every check of this length, it has not fallen by ackFallC ...
    float ackFallC = 0.1f;
    uint32_t ackMaxMs = 600000UL;         // ... and in any case after this long
  };

  void reset() { sounding_ = false; muted_ = false; }
  // Every cycle. `active` = the alarm itself (High: highTemperatureActive_; Emergency: emergencyActive_).
  // heaterOff: the machine has nothing left to cut (request 0): an ACK is then not asked to be followed by a fall of the reading (a hot day: it cannot fall).
  void update(uint32_t now, bool valid, float pv, bool active, const Params &p, bool heaterOff = false) {
    if (!valid || !std::isfinite(pv) || !active) { reset(); return; }
    if (!sounding_) { sounding_ = true; muted_ = false; soundAt_ = now; return; }
    if (muted_) {
      if (pv >= ackPv_ + p.ackRiseC) {
        muted_ = false;
      } else if (static_cast<uint32_t>(now - muteStart_) >= p.ackMaxMs) {
        muted_ = false;
      } else if (static_cast<uint32_t>(now - ackAt_) >= p.ackCheckMs) {
        if (heaterOff) ackAt_ = now;                                               // nothing more to cut: stay quiet (the rise and the time limits above still apply)
        else if (pv > ackPv_ - p.ackFallC) muted_ = false;                         // not falling: it speaks again
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
  bool sounding_ = false, muted_ = false;
  uint32_t soundAt_ = 0U, muteStart_ = 0U, ackAt_ = 0U;
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
