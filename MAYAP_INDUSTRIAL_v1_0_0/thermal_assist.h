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

// Smallest hold (% of the bank) that is fed forward when the hold was MEASURED by an accepted Smart AutoTune (an ONLINE estimate keeps the
// larger Policy::HoldFfMinPct, because a 0.1 C probe hides 100 x 0.1 / (gain x window) duty points and a feed-forward inside that noise only
// perturbs a limit cycle). On a well insulated oven (hold 1-3 %) the feed-forward matters: without it the integral alone carries the hold, and
// every time the startup brake closes for a minute after a probe step the integral limit (which follows that transient ceiling) wipes it, so
// the loop runs proportional-only and parks about hold / Kp (~0.15 C) under the set point. A measured hold is good to ~0.2 point, and the
// feed-forward is bounded (95 % of the hold, ramped in with the confidence).
constexpr float HoldFfMeasuredMinPct = 1.0f;

struct Assist {  // what the PID receives
  float feedForward = 0;             // % duty (hold): bumpless, the integral gives way as this moves
  float addForward = 0;              // % duty (known disturbance, e.g. vent): ADDED, not traded with the integral
  bool freezeIntegral = false;       // integral holds (it may still unwind when PV is high)
  float integralCeiling = INFINITY;  // absolute bound on the integral (vent wind-up guard)
  // Ceiling (never a floor) on the integral gain, derived from the learned delay and gain.
  // It can only LOWER the configured / AutoTuned Ki, never raise it. INFINITY == no limit.
  float kiMax = INFINITY;
  // Ceiling on the proportional gain (and with it the derivative gain: Td is kept), from the measured gain and delay: Kp x gain x (delay + lag)
  // beyond ~1 is a limit cycle. Only ever LOWERS the configured / AutoTuned Kp. INFINITY == no limit.
  float kpMax = INFINITY;
  // Authority limiter on the PID CORRECTION around the feed-forward: the output stays inside
  // [ff - A, ff + A], A = corrBase + corrPerC * |error|. It only NARROWS the legacy [0, max]
  // range, so a limit cycle through a long delay cannot exceed an amplitude the learned plant
  // would turn into more ripple than the budget, while a large error still gets full authority.
  // INFINITY == off.
  float corrBase = INFINITY;
  float corrPerC = 60.0f;
};

// Launch plan: what the startup controller measured on the LAST good warm-up of this oven, so the next batch can launch at full power at once
// instead of probing. It is a PRIOR, never evidence: the controller re-measures the heater gain during every launch (a changed number of egg
// trolleys changes the thermal mass, hence the gain) and drops the plan for the rest of the batch if the oven does not answer as promised.
struct LaunchPlan {
  bool valid = false;
  float gain = 0;       // degC/s at 100 % duty, as measured during the launch (the loss makes it read a little low)
  float delaySec = 0;   // delay after which the heat of an ON-second starts to reach PV (the arrival kernel's, measured by the launch)
  float coastCal = 1;   // measured drift after a cut / the heat-in-flight model (1 = the model is right)
  bool calMeasured = false;   // coastCal was measured on a real cut (false = the conservative default)
  float holdPct = 0;    // duty that holds PV at SP, measured from the gain (0 = unknown); a prior credited at 80 %
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
