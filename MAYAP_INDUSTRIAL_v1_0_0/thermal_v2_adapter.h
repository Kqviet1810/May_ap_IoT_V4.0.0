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
  float capPct = 0.0f;                // configured heater power ceiling [%]
  float headroomC = 0.0f;             // from the target to the vent / High line, in the control sensor frame
  float alarmHeadroomC = 0.0f;        // from the target to the Emergency line
  float deliveredPct = 0.0f;          // what the arbiter really gave in the last step (0 or 100)
  float e115Frac = 0.0f;              // E115 evidence: heater-on time without the 0.3 degC rise / its limit
  float dQ = 0.1f;                    // sensor step [degC]: 0.1 (X10), 0.01 (X100 / native)
  float dtOverrideS = NAN;            // host rigs only (jitter experiments); never set by the firmware
};

class ThermalV2Slot {
 public:
  float step(const ThermalV2SlotIn &in) {
    float dt = 0.1f;
    if (isfinite(in.dtOverrideS)) dt = in.dtOverrideS;
    else if (haveLast_) dt = static_cast<float>(static_cast<uint32_t>(in.nowMs - lastMs_)) * 0.001f;     // unsigned difference: millis() roll-over safe
    lastMs_ = in.nowMs; haveLast_ = true;
    thermalv2::Input i;
    i.pv = in.pv; i.sp = in.sp; i.dQ = in.dQ; i.headroomC = in.headroomC; i.alarmHeadroomC = in.alarmHeadroomC; i.capPct = in.capPct;
    i.deliveredPct = in.deliveredPct; i.e115Frac = in.e115Frac; i.dtS = dt; i.newSample = in.newSample; i.permit = in.permit;
    last_ = ctl_.step(i);
    float u = last_.powerPct;
    if (!(u > 0.0f)) u = 0.0f;                                                     // also catches NaN
    const float cap = (isfinite(in.capPct) && in.capPct > 0.0f) ? in.capPct : 0.0f;
    if (u > cap) u = cap;
    return u > 100.0f ? 100.0f : u;
  }
  void reset() { ctl_.reset(); haveLast_ = false; last_ = thermalv2::Output(); }
  const thermalv2::Output &last() const { return last_; }
  thermalv2::Controller &controller() { return ctl_; }
  const thermalv2::Controller &controller() const { return ctl_; }
 private:
  thermalv2::Controller ctl_;
  thermalv2::Output last_;
  uint32_t lastMs_ = 0; bool haveLast_ = false;
};
