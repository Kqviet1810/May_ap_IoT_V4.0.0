#pragma once
// What makes a noise, and when it stops. Pure C++ (no Arduino types): MachineController feeds these every cycle and tests/alarm-sound.cpp drives the
// very same code.
//
// The protective actions of the over-temperature lines (heater cut, fans forced) belong to the thresholds the owner sets and keep working exactly as
// before. The NOISE is decoupled from those thresholds: an owner who sets the High / Emergency lines a tenth of a degree over the vent line (37.7 /
// 37.8 on SV 37.5) must not have the loud siren for an excursion of 0.2 degC that the heater and the fans are already dealing with.
//
//   Sound classes (decided per fault code in machine_control.h, faultSoundClass):
//     Siren  - the external siren and the buzzer: Emergency temperature (EmergencyVoice), output conflict
//     Alert  - the HMI buzzer until ACK or until the condition is gone: the faults that stop the heater (sensor, heater not heating, storage ...)
//     Chime  - three short beeps when it appears, a thin reminder while it lasts, no ACK: warnings that need someone, not now
//     Silent - shown on the HMI / Web / push only: diagnostics
//   High temperature (HighVoice) is a Chime that speaks again only when it gets worse.
//   Every sound follows the LIVE condition, never the stored record: a latched fault that is gone is silent at once (the record stays for ACK).
#include <cmath>
#include <cstdint>

namespace alarmsound {

class EmergencyVoice {
 public:
  struct Params {
    float dangerAboveSvC = 0.8f;          // siren at once at/over SV + this (and over the Emergency line): a danger whatever the line was set to
    uint32_t persistMs = 180000UL;        // ... or when the line has been crossed this long (the heater is cut, the fans run, and it does not come down)
    float riseC = 0.3f;                   // ... or when it has risen this much since the line was crossed
    uint32_t steadyMs = 60000UL;          // the siren is continuous this long, then pulsed ...
    uint32_t onMs = 5000UL, offMs = 25000UL;   // ... on / off
    float offBelowC = 0.1f;               // sound ends when the reading is this far under the line for offConfirmMs (the lock of the heater has its own, longer, hysteresis)
    uint32_t offConfirmMs = 2000UL;
    float rearmC = 0.1f;                  // after such an end, it speaks again only if the reading climbs this far over the line
    float ackRiseC = 0.2f;                // ACK mutes; the siren is back when the reading rose this much since the ACK ...
    uint32_t ackCheckMs = 120000UL;       // ... or when, at every check of this length, it has not fallen by ackFallC ...
    float ackFallC = 0.1f;
    uint32_t ackMaxMs = 600000UL;         // ... and in any case after this long
  };

  void reset() {
    raised_ = false; quiet_ = false; stage_ = false; muted_ = false; chime_ = false; offOn_ = false;
  }
  // Every cycle. `active` = the Emergency line itself (the protective one).
  void update(uint32_t now, bool valid, float pv, float sp, float line, bool active, const Params &p) {
    if (!valid || !std::isfinite(pv) || !active) { reset(); return; }
    if (pv < line - p.offBelowC) {
      if (!offOn_) { offOn_ = true; offSince_ = now; }
    } else {
      offOn_ = false;
    }
    if (offOn_ && static_cast<uint32_t>(now - offSince_) >= p.offConfirmMs) {      // cooled: silence now, whatever the lock of the heater still does
      quiet_ = true; raised_ = false; stage_ = false; muted_ = false; offOn_ = false;
      return;
    }
    if (quiet_) {
      if (pv >= line + p.rearmC) quiet_ = false; else return;
    }
    if (!raised_) {
      raised_ = true; raisedAt_ = now; crossPv_ = pv; stage_ = false; muted_ = false;
      evaluateStage(now, pv, sp, line, p);
      if (!stage_) chime_ = true;                                                  // the heater and the fans are at work; the owner gets a quiet note first
      return;
    }
    evaluateStage(now, pv, sp, line, p);
    if (stage_ && muted_) {
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
    if (!stage_ || muted_) return;
    muted_ = true; muteStart_ = ackAt_ = now; ackPv_ = pv;
  }
  bool consumeChime() { const bool c = chime_; chime_ = false; return c; }
  bool stage() const { return stage_; }
  bool audible() const { return stage_ && !muted_; }
  bool muted() const { return muted_; }
  // The external siren at this moment (continuous at first, then pulsed).
  bool siren(uint32_t now, const Params &p) const {
    if (!audible()) return false;
    const uint32_t el = static_cast<uint32_t>(now - stageAt_);
    if (el < p.steadyMs) return true;
    const uint32_t period = p.onMs + p.offMs;
    return period == 0U || ((el - p.steadyMs) % period) < p.onMs;
  }

 private:
  void evaluateStage(uint32_t now, float pv, float sp, float line, const Params &p) {
    if (stage_) return;
    const float danger = line > sp + p.dangerAboveSvC ? line : sp + p.dangerAboveSvC;
    if (pv >= danger || static_cast<uint32_t>(now - raisedAt_) >= p.persistMs || pv >= crossPv_ + p.riseC) {
      stage_ = true; stageAt_ = now;
    }
  }
  bool raised_ = false, quiet_ = false, stage_ = false, muted_ = false, chime_ = false, offOn_ = false;
  uint32_t raisedAt_ = 0U, stageAt_ = 0U, offSince_ = 0U, muteStart_ = 0U, ackAt_ = 0U;
  float crossPv_ = 0.0f, ackPv_ = 0.0f;
};

// The High line: three beeps when it appears, then silence. It speaks again only when it gets worse.
class HighVoice {
 public:
  struct Params {
    float riseC = 0.2f;                   // risen this far since the last note
    uint32_t stallMs = 600000UL;          // or not fallen by fallC over this long
    float fallC = 0.1f;
    uint32_t minGapMs = 300000UL;         // never more often than this
  };
  void reset() { raised_ = false; acked_ = false; pending_ = false; }
  void update(uint32_t now, bool valid, float pv, bool active, const Params &p) {
    if (!valid || !std::isfinite(pv) || !active) { reset(); return; }
    if (!raised_) {
      raised_ = true; acked_ = false;
      note(now, pv);
      refPv_ = pv; refAt_ = now;
      return;
    }
    if (acked_) return;
    const bool gapOk = static_cast<uint32_t>(now - lastAt_) >= p.minGapMs;
    if (pv >= lastPv_ + p.riseC && gapOk) note(now, pv);
    if (static_cast<uint32_t>(now - refAt_) >= p.stallMs) {
      if (pv > refPv_ - p.fallC && gapOk) note(now, pv);
      refPv_ = pv; refAt_ = now;
    }
  }
  void ack() { acked_ = true; pending_ = false; }
  bool consumeAlert() { const bool a = pending_; pending_ = false; return a; }

 private:
  void note(uint32_t now, float pv) { pending_ = true; lastAt_ = now; lastPv_ = pv; }
  bool raised_ = false, acked_ = false, pending_ = false;
  uint32_t lastAt_ = 0U, refAt_ = 0U;
  float lastPv_ = 0.0f, refPv_ = 0.0f;
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
