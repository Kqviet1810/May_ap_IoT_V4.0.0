#pragma once
// What makes a noise, and when it stops. Pure C++ (no Arduino types): MachineController feeds these every cycle and tests/alarm-sound.cpp drives the
// very same code.
//
//   Sound classes (decided per fault code in machine_control.h, faultSoundClass):
//     Siren  - the external siren and the buzzer: the over-temperature siren (OverSetpointSiren) and an output that does not obey
//     Alert  - the HMI buzzer until ACK or until the condition is gone: the faults that stop the heater (sensor, heater not heating, storage ...)
//     Chime  - three short beeps when it appears, a thin reminder while it lasts, no ACK: warnings that need someone, not now
//     Silent - shown on the HMI / Web / push only: diagnostics
//   Every sound follows the LIVE condition, never the stored record: a latched fault that is gone is silent at once (the record stays for ACK).
//
//   2.9.11: the over-temperature siren is ONE rule measured from the setpoint (OverSetpointSiren below): 0.2 degC over SV for 60 s, or 0.3 degC over at
//   once; continuous; silent when the reading has been within 0.1 degC over SV for 60 s. No ACK, no mute, no pulsing, no special level. (Before: a gate on
//   the E111 line with a 60 s dwell, a 5 min continuous / 5 s on / 25 s off voice, an ACK that muted it for up to 10 min, a lost sample that reset it.)
#include <cmath>
#include <cstdint>

namespace alarmsound {

// The siren of the over-temperature: ONE rule, measured from the SETPOINT (over = filtered reading - SV), nothing else.
//   on : over >= onC for onDelayMs without a break, or over >= immediateC at once
//   off: over <= offC for offDelayMs without a break
//   Continuous while on. No ACK, no mute, no pulsing, no special level: it stops only when the condition has ended (ISA-18.2 style on-delay / off-delay / deadband).
//   A sample that is not valid (sensor) neither starts nor stops anything: the state is kept (a siren that sounds does not fall silent because one reading was lost).
//   Not eligible (no batch and the alarm without batch is off): silent.
// The protective actions of E111 / E112 (heater, contactor, fans) do not go through here.
class OverSetpointSiren {
 public:
  struct Params {
    float onC = 0.2f, immediateC = 0.3f, offC = 0.1f;
    uint32_t onDelayMs = 60000UL, offDelayMs = 60000UL;
    float epsC = 0.005f;                    // a sensor step of 0.1 compared as a float: 0.2 means 0.2
  };
  void reset() { on_ = false; overSince_ = calmSince_ = 0U; haveOver_ = haveCalm_ = false; }
  // `over` is NAN when the reading is not valid.
  bool update(uint32_t now, float over, bool eligible, const Params &p) {
    if (!eligible) { reset(); return false; }
    if (!std::isfinite(over)) return on_;
    if (over >= p.immediateC - p.epsC) { on_ = true; }
    if (over >= p.onC - p.epsC) {
      if (!haveOver_) { haveOver_ = true; overSince_ = now; }
      if (static_cast<uint32_t>(now - overSince_) >= p.onDelayMs) on_ = true;
    } else {
      haveOver_ = false;
    }
    if (over <= p.offC + p.epsC) {
      if (!haveCalm_) { haveCalm_ = true; calmSince_ = now; }
      if (on_ && static_cast<uint32_t>(now - calmSince_) >= p.offDelayMs) { on_ = false; haveOver_ = false; }
    } else {
      haveCalm_ = false;
    }
    return on_;
  }
  bool on() const { return on_; }

 private:
  bool on_ = false, haveOver_ = false, haveCalm_ = false;
  uint32_t overSince_ = 0U, calmSince_ = 0U;
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
