#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// Climb assist: a second, separate controller that owns ONE situation - the oven stands under SV and does not come up. It looks at nothing but the
// measured PV (its fine value and its measured slope) and at the power it has itself asked for; no oven model, no kAir, no heat-in-flight estimate.
//
// Arms when PV has been more than ARM_GAP_C under SV and has not been rising at the speed that would close the gap in CLOSE_S seconds, for STALL_S
// seconds in a row. While armed it raises its own power floor: every second it adds (wanted slope - measured slope) / (REF_GAIN * RAISE_S) percent,
// where the wanted slope is gap / CLOSE_S. Released as soon as PV is within RELEASE_GAP_C of SV, or the slope it measures would carry PV to SV within
// ARRIVE_S seconds, or the door/vent fan/protection takes over. The caller then gives the power floor to the hold controller as its hold level.
namespace thermalclimb {

constexpr float ARM_GAP_C = 0.20f;         // under SV by more than this ...
constexpr float STALL_S = 120.0f;          // ... and not closing for this long
#ifndef STALL_FRAC
#define STALL_FRAC 0.15f
#endif
constexpr float STALL_SLOPE_FRAC = STALL_FRAC;       // "not closing" = under this fraction of the closing speed (a brake near SV is slower than 0.5 but not flat)
constexpr float CLOSE_S = 300.0f;          // the gap should close in this time
constexpr float RELEASE_GAP_C = 0.08f;
constexpr float ARRIVE_S = 240.0f;         // slope x this >= gap: PV is on its way, let go
constexpr float REF_GAIN = 1.2e-4f;        // degC/s per percent: the gain of a slow oven (a faster oven only closes the gap sooner)
constexpr float RAISE_S = 300.0f;          // integral time of the raise
constexpr float MAX_GAP_C = 1.2f;          // farther than this the climb controller of the hold law does the work

struct Out { bool active = false; float floorPct = 0.0f; bool released = false; float releaseHold = 0.0f; };

class Assist {
 public:
  void reset() { stallS_ = 0.0f; active_ = false; floor_ = 0.0f; }
  // Call every control step. `cmdPct` = what the hold law asks for now (the floor starts there). `slope` = measured PV slope [degC/s] (NaN: unknown).
  Out step(float pv, float sp, float slope, float cmdPct, float capPct, bool blocked, float dtS) {
    Out o;
    const float gap = sp - pv;
    if (blocked || !std::isfinite(gap) || !std::isfinite(slope)) { if (active_) { o.released = true; o.releaseHold = std::max(0.0f, floor_ - std::max(0.0f, std::isfinite(slope) ? slope : 0.0f) / REF_GAIN); } reset(); return o; }
    const float s = std::max(0.0f, slope);
    if (!active_) {
      const bool stalled = gap > ARM_GAP_C && gap < MAX_GAP_C && s < STALL_SLOPE_FRAC * gap / CLOSE_S;
      stallS_ = stalled ? stallS_ + dtS : 0.0f;
      if (stallS_ >= STALL_S) { active_ = true; floor_ = cmdPct; secAcc_ = 0.0f; }
      return o;
    }
    if (gap <= RELEASE_GAP_C || gap >= MAX_GAP_C) {
      // what the air still gains with the floor is not load: take it off
      o.released = true; o.releaseHold = std::max(0.0f, floor_ - s / REF_GAIN); reset(); return o;
    }
    secAcc_ += dtS;
    if (secAcc_ >= 1.0f) {
      secAcc_ -= 1.0f;
      const float want = gap / CLOSE_S;
      if (s < want) floor_ = std::min(capPct, floor_ + (want - s) / (REF_GAIN * RAISE_S));
    }
    o.active = true; o.floorPct = floor_;
    return o;
  }
  bool active() const { return active_; }
 private:
  float stallS_ = 0.0f, floor_ = 0.0f, secAcc_ = 0.0f;
  bool active_ = false;
};

}  // namespace thermalclimb
