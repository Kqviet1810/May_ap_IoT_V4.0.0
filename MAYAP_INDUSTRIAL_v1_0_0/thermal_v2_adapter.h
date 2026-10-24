#pragma once
// Thermal V2.0.0 slot adapter.  Pure C++ (no Arduino types): the host rig compiles THIS text, so what the digital twin runs is what the firmware runs.
// It turns the facts the production heating route already knows into thermalv2::Input, derives the step time from the control-task clock and applies the
// last clamp (NaN / negative -> 0, never above the configured cap).  It owns no protection: the permit it receives is decided by the production layers.
#include <math.h>
#include <stdint.h>
#include "thermal_v2.h"

struct ThermalV2SlotIn {
  uint32_t nowMs = 0;                 // control-task clock; the step time is the difference between two calls (clamped by the controller)
  float pv = NAN, sp = NAN;           // control temperature (offset applied) and target [degC]
  bool newSample = false;             // a new accepted sensor sample arrived since the last call
  bool permit = false;                // production decided the SSR may conduct (all protection, interlocks and actuator readiness)
  bool shadow = false;                // permit is false ONLY because the vent line blocks the heater: the controller keeps running and commands 0
  float capPct = 0.0f;                // configured heater power ceiling [%]
  float headroomC = 0.0f;             // from the target to the vent / High line, in the control sensor frame
  float alarmHeadroomC = 0.0f;        // from the target to the Emergency line
  float deliveredPct = 0.0f;          // what the arbiter really gave in the last step (0 or 100)
  float e115Frac = 0.0f;              // E115 evidence: heater-on time without the 0.3 degC rise / its limit
  float dQ = 0.1f;                    // sensor step [degC]: 0.1 (X10), 0.01 (X100 / native)
  bool ventFan = false;               // the exhaust fan is running (the arbiter's real output)
  float ventOnInS = 1.0e6f, ventOffInS = 1.0e6f;   // the vent schedule's own knowledge: seconds to the next start / to the end of the run (1e6 = unknown)
  float dtOverrideS = NAN;            // host rigs only (jitter experiments); never set by the firmware
};

// The control task runs every 5 ms; the controller is designed and tested for a 100 ms step (the rig steps at 100 ms), and one learning step costs
// milliseconds on the ESP32.  So the controller runs at most every STEP_PERIOD_MS while the permit stays true; in between the last answer is held
// (clamped to the current cap).  A change of the permit, or a new sensor sample that has been waiting, is never held back.
class ThermalV2Slot {
 public:
  static constexpr uint32_t STEP_PERIOD_MS = 100U;
  float step(const ThermalV2SlotIn &in) {
    const bool live = in.permit || in.shadow;
    const bool hold = haveLast_ && live && lastLive_ && in.permit == lastPermit_ && !isfinite(in.dtOverrideS) &&
                      static_cast<uint32_t>(in.nowMs - lastMs_) < STEP_PERIOD_MS;
    if (hold) {
      if (in.newSample) pendingSample_ = true;                                       // the next real step must see it
      return clampOut(held_, in.capPct);
    }
    float dt = 0.1f;
    if (isfinite(in.dtOverrideS)) dt = in.dtOverrideS;
    else if (haveLast_) dt = static_cast<float>(static_cast<uint32_t>(in.nowMs - lastMs_)) * 0.001f;     // unsigned difference: millis() roll-over safe
    lastMs_ = in.nowMs; haveLast_ = true; lastPermit_ = in.permit; lastLive_ = live;
    thermalv2::Input i;
    i.pv = in.pv; i.sp = in.sp; i.dQ = in.dQ; i.headroomC = in.headroomC; i.alarmHeadroomC = in.alarmHeadroomC; i.capPct = in.capPct;
    i.deliveredPct = in.deliveredPct; i.e115Frac = in.e115Frac; i.dtS = dt; i.newSample = in.newSample || pendingSample_; i.permit = in.permit; i.shadow = in.shadow;
    i.ventFan = in.ventFan; i.ventOnInS = in.ventOnInS; i.ventOffInS = in.ventOffInS;
    pendingSample_ = false;
    last_ = ctl_.step(i);
    held_ = clampOut(last_.powerPct, in.capPct);
    return held_;
  }
  void reset() { ctl_.reset(); haveLast_ = false; lastPermit_ = false; lastLive_ = false; pendingSample_ = false; held_ = 0.0f; last_ = thermalv2::Output(); }
  const thermalv2::Output &last() const { return last_; }
  thermalv2::Controller &controller() { return ctl_; }
  const thermalv2::Controller &controller() const { return ctl_; }
 private:
  static float clampOut(float u, float capPct) {
    if (!(u > 0.0f)) return 0.0f;                                                  // also catches NaN
    const float cap = (isfinite(capPct) && capPct > 0.0f) ? capPct : 0.0f;
    if (u > cap) u = cap;
    return u > 100.0f ? 100.0f : u;
  }
  thermalv2::Controller ctl_;
  thermalv2::Output last_;
  uint32_t lastMs_ = 0; bool haveLast_ = false, lastPermit_ = false, lastLive_ = false, pendingSample_ = false; float held_ = 0.0f;
};
