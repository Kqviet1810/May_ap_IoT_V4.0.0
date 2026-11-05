#pragma once
// The hold controller (thermal_v3.h) with the climb assist (thermal_climb.h) beside it, as MachineController will run them.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "../MAYAP_INDUSTRIAL_v1_0_0/thermal_climb.h"
struct DualCtl {
  static constexpr bool kFineInput = true;
  thermalv3::Controller c;
  thermalclimb::Assist a;
  float slopeF = 0.0f;
  thermalv3::Output step(const thermalv3::Input &in) {
    thermalv3::Output o = c.step(in);
    const float dt = in.dtS;
    if (std::isfinite(in.slopeFine)) slopeF += (in.slopeFine - slopeF) * dt / (60.0f + dt);
    const bool blocked = in.ventFan || in.shadow || !in.permit || c.state() == thermalv3::StDoor || c.state() == thermalv3::StFrozen;
    const thermalclimb::Out r = a.step(in.pv, in.sp, slopeF, o.powerPct, c.pHold(), in.capPct, blocked, dt);
    if (r.released) { if (getenv("DBGREL")) fprintf(stderr,"release hold %.1f (hold before %.1f) slopeF %g gap %.3f\n", r.releaseHold, c.pHold(), slopeF, in.sp-in.pv); if (r.releaseHold > 0.0f) c.adoptHold(r.releaseHold); }
    if (r.active && r.floorPct > o.powerPct) o.powerPct = r.floorPct;
    return o;
  }
  float pHold() const { return c.pHold(); }
  float kAir() const { return c.kAir(); }
  float tauTransit() const { return c.tauTransit(); }
  void restore(float k, float t) { c.restore(k, t); }
};
