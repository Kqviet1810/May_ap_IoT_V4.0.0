#pragma once
// =========================================================================================
// Thermal V3 Slot Adapter
// Pure C++11 (no Arduino types).
// Connects production machine_control / task tick to thermalv3::Controller.
// =========================================================================================
#include <cmath>
#include <cstdint>
#include "thermal_v3.h"

struct ThermalV3SlotIn {
  uint32_t nowMs = 0;                 // Control-task clock
  float pv = NAN, sp = NAN;           // Control temperature (offset applied) and target [degC]
  bool permit = false;                // Production decided SSR may conduct (all protections green)
  bool shadow = false;                // Permit is false only because vent line blocks heater
  float capPct = 0.0f;                // Configured heater power ceiling [%]
  float dQ = 0.1f;                    // Sensor resolution step [degC]
  float dtOverrideS = NAN;            // Host rigs only
};

class ThermalV3Slot {
 public:
  static constexpr uint32_t STEP_PERIOD_MS = 100U;

  float step(const ThermalV3SlotIn &in) {
    const bool live = in.permit || in.shadow;
    const bool hold = haveLast_ && live && lastLive_ && in.permit == lastPermit_ && !std::isfinite(in.dtOverrideS) &&
                      static_cast<uint32_t>(in.nowMs - lastMs_) < STEP_PERIOD_MS;
    if (hold) {
      return clampOut(held_, in.capPct);
    }

    float dt = 0.1f;
    if (std::isfinite(in.dtOverrideS)) {
      dt = in.dtOverrideS;
    } else if (haveLast_) {
      dt = static_cast<float>(static_cast<uint32_t>(in.nowMs - lastMs_)) * 0.001f;
    }

    lastMs_ = in.nowMs;
    haveLast_ = true;
    lastPermit_ = in.permit;
    lastLive_ = live;

    thermalv3::Input i;
    i.pv = in.pv;
    i.sp = in.sp;
    i.dQ = in.dQ;
    i.capPct = in.capPct;
    i.dtS = dt;
    i.permit = in.permit;
    i.shadow = in.shadow;

    last_ = ctl_.step(i);
    held_ = clampOut(last_.powerPct, in.capPct);
    return held_;
  }

  void reset() {
    ctl_.reset();
    haveLast_ = false;
    lastPermit_ = false;
    lastLive_ = false;
    held_ = 0.0f;
    last_ = thermalv3::Output();
  }

  const thermalv3::Output &last() const { return last_; }
  thermalv3::Controller &controller() { return ctl_; }
  const thermalv3::Controller &controller() const { return ctl_; }

 private:
  static float clampOut(float u, float capPct) {
    if (!(u > 0.0f)) return 0.0f;
    const float cap = (std::isfinite(capPct) && capPct > 0.0f) ? capPct : 0.0f;
    if (u > cap) u = cap;
    return u > 100.0f ? 100.0f : u;
  }

  thermalv3::Controller ctl_;
  thermalv3::Output last_;
  uint32_t lastMs_ = 0;
  bool haveLast_ = false;
  bool lastPermit_ = false;
  bool lastLive_ = false;
  float held_ = 0.0f;
};
