#pragma once
#include <cmath>
#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_climb_simple.h"
struct SimpleCtl {
  static constexpr bool kFineInput = true;
  thermalv3::Controller c;
  thermalsimple::Climb g;
  thermalv3::Output step(const thermalv3::Input &in) {
    thermalv3::Output o = c.step(in);
    const bool blocked = in.ventFan || in.shadow || !in.permit || c.state() == thermalv3::StDoor || c.state() == thermalv3::StFrozen;
    const thermalsimple::Out r = g.step(in.pv, in.sp, in.slopeFine, o.powerPct, in.capPct, c.tauTransit(), c.kAir(), blocked, in.dtS);
    if (r.released) c.adoptHold(r.releaseHold, true);
    if (r.active) o.powerPct = r.powerPct;
    return o;
  }
  float pHold() const { return c.pHold(); }
  float kAir() const { return c.kAir(); }
  float tauTransit() const { return c.tauTransit(); }
  void restore(float k, float t) { c.restore(k, t); }
};
