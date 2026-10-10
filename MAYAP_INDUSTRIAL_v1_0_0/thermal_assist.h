#pragma once
// Interface structs between Adaptive Thermal V1 and the legacy thermal algorithms.
// Defaults reproduce the legacy controller exactly: an all-default Assist / StartupHint
// is a no-op, which is how every pre-existing caller and test keeps its behaviour.
#include <cmath>

// Smart Thermal program switch. 0 (default, production until qualified and approved) = bit-identical Adaptive V1 / legacy PID.
// 1 enables every Smart Thermal change at once (startup heat-in-flight brake, learner hold convergence). Each part also has its own
// run-time setter for A/B tests. Rollback is rebuilding with 0.
#ifndef MAYAP_SMART_THERMAL
#define MAYAP_SMART_THERMAL 0
#endif

namespace MayapThermal {

struct Assist {  // what the PID receives
  float feedForward = 0;             // % duty (hold): bumpless, the integral gives way as this moves
  float addForward = 0;              // % duty (known disturbance, e.g. vent): ADDED, not traded with the integral
  bool freezeIntegral = false;       // integral holds (it may still unwind when PV is high)
  float integralCeiling = INFINITY;  // absolute bound on the integral (vent wind-up guard)
  // Ceiling (never a floor) on the integral gain, derived from the learned delay and gain.
  // It can only LOWER the configured / AutoTuned Ki, never raise it. INFINITY == no limit.
  float kiMax = INFINITY;
  // Authority limiter on the PID CORRECTION around the feed-forward: the output stays inside
  // [ff - A, ff + A], A = corrBase + corrPerC * |error|. It only NARROWS the legacy [0, max]
  // range, so a limit cycle through a long delay cannot exceed an amplitude the learned plant
  // would turn into more ripple than the budget, while a large error still gets full authority.
  // INFINITY == off.
  float corrBase = INFINITY;
  float corrPerC = 60.0f;
};

struct StartupHint {  // learned plant knowledge for the startup/braking controller
  bool valid = false;
  float coastPerOnMs = 0;  // degC of eventual rise per ms of full-power ON (includes a safety margin)
  float gainPerSec = 0;    // learned degC/s at 100 % duty
  float ventPct = 0;       // extra duty that exactly offsets a KNOWN, current/imminent vent cooling
  bool slopeValid = false;
  float slopePerSec = 0;   // window-regression slope of PV (robust to 0.1 C quantization)
  float delaySec = 0;
  float holdPct = 0;
  float strength = 0;      // 0..1
};

}  // namespace MayapThermal
