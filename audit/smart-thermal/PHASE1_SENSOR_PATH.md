# Smart Thermal — Phase 1: simulator integrity (sensor path)

SIMULATION ONLY. No threshold, GPIO, polarity, fail-safe behaviour or production header was changed in this phase.
Files: `tests/thermal-sensor-path.{h,cpp}` (new), `tests/thermal-adaptive-sim.h` (opt-in `Scenario::path`, off by default),
`tools/test_thermal_control.py` (additive: 6 more production-text extractions), `tools/test_thermal_sensor_path.py` (gate),
`tests/thermal-sensor-path-baseline.json` (ratchet), `.github/workflows/reliability-checks.yml` (+1 step),
`audit/smart-thermal/phase1-sensor-path-adaptive.csv` (per-case evidence).

## What was wrong with the old harness
`thermal-adaptive-sim.h` derived `highTemperatureActive_`, `emergencyActive_` and the controller's `faults_.inhibit/drop`
from the **plant true temperature** (instantaneous, no confirmation timer, no probe lag, no quantisation) and handed the
controller the true temperature as its "raw" sample. High/Emergency columns in the 788/2160 matrices are therefore
plant-truth counters; they say nothing about whether the firmware's own safety path trips.

## What the new path does
Plant → first-order probe lag → fault injection → quantisation → production median/IIR filter → **production**
`processSensor` acceptance block (1.5 °C plausibility + suspect latch + frozen reference) → **production** `updateAlarms`
(High 1 s confirm / Emergency on `max(raw, filtered)`, clear hysteresis) → **production** E115 energy accounting and E104 →
inhibit/drop mapping (Stop-severity rows of the fault table) → the unchanged heating route and OutputArbiter.
The six code blocks are extracted from `machine_control.h` by marker at build time (`--emit-includes`); the only textual
edit is `sensor_.dataValid()` → a plain member. Plant truth is only an observer. Three truths are reported per case:
plant true (`truePeak`, `trueHighS`, `trueEmergencyS`, `onWhileTrueHighS`), firmware saw (`fwHigh`, `fwEmergency`, `e115S`,
`e104S`, `sensorLostS`), command (`unsafeCommandTicks`: SSR commanded ON while the firmware's own inhibit/drop was asserted).
Actuator faults (SSR conducting although commanded OFF; SSR + contactor both stuck) keep the sensor healthy and are applied
in the plant only; the firmware has **no feedback input** for either (`outputs_.state()` is the command).

Regression safety of the change itself: with `path.enabled=false` the 788, 2160, vent(63×3), hardware-change and fault
matrices are **byte-identical** to the frozen baseline (`cmp` on all five CSVs); ASan+UBSan clean on the new suite;
`test_thermal_control.py --quick` (adaptive-unit 737 checks, smart-autotune-unit 471 checks) still passes.

## Result: 60 cases (4 plants × 15 scenarios), Adaptive V1, SP 37.5, High 38.2, Emergency 39.0
| fault | cases | verdicts | worst plant-true peak °C |
|---|---|---|---|
| none (lag 0/10/30/60 s, offset −0.5) | 20 | OK 16, **FALSE_TRIP_NO_FAULT 4** | 38.07 |
| disconnect 120 s | 4 | SAFE_UNDER_FAULT 4 | 37.59 |
| spike +3 °C (1 sample) | 4 | SAFE_UNDER_FAULT 4 (trips High/Emergency on one sample: fail-safe, availability cost) | 37.59 |
| stuck_high 40 °C | 4 | SAFE_UNDER_FAULT 4 | 37.59 |
| stuck_low (30.0 / 36.5) | 8 | **TRUE_VIOLATION_STOPPED_LATE_BY_E115_E104 8** | **77.62** |
| frozen value | 4 | UNDETECTED_TRUE_VIOLATION 3, STOPPED_LATE 1 | 47.45 |
| drift low (0.5 / 2 °C·h⁻¹) | 8 | UNDETECTED_TRUE_VIOLATION 7, SAFE 1 | 40.56 |
| SSR stuck ON, contactor works | 4 | **TRUE_VIOLATION_DETECTED_LATE 4** (High re-trips 10–25×; plant ≥ High for 2500–3600 s, ≥ Emergency up to 2264 s) | 40.21 |
| SSR **and** contactor stuck | 4 | **ACTUATOR_UNSTOPPABLE_BY_FIRMWARE 4** | 108.56 |
UNSAFE_COMMAND = 0 in all 60 cases: whenever the firmware knows it is unsafe, the heater is OFF in the same cycle.

## Findings (none is "fixed" here; all need a decision)
* **S1 (P1) single-sensor limits are real.** A stuck-low probe (valid frames, plausible after the 3-sample suspect
  confirmation, or inside the 1.5 °C window) leaves the PID heating until E115 (900 s of accumulated ON) / E104 (20 min):
  the plant reaches 40.9–77.6 °C. Frozen and slow-drift probes stay undetected whenever the reading is inside
  `[SP − hysteresis, SP)` because E115/E104 evidence only accumulates while the reported PV is below `SP − hysteresis`.
  Software cannot close this; the independent hardware over-temperature cut-out (SAFETY_HARDWARE_REQUIREMENTS.md) is the
  only protection and is a commissioning precondition. This simulation does **not** replace it.
* **S2 (P1) SSR stuck ON with a working contactor cycles around High**: the master drops at High, re-closes after the
  10 s clear confirmation, the stuck SSR heats again. Result: tens of re-trips and 40–60 min above High (Emergency reached on
  two plants). Proposed (needs approval, not implemented): latch the master after N High trips within a window until
  operator acknowledge.
* **S3 (P1) SSR + contactor both stuck**: uncontrollable by firmware (108 °C). Hardware cut-out only.
* **S4 (P2, controller quality) nuisance E115 under probe lag ≥ 30 s**: 4 of 16 fault-free lag cases stop the heater for the
  rest of the batch (`light_d15` lag 30/60, `medium_d15` lag 30, `medium_d60` lag 30; MAE 9–17 °C afterwards). Cause: the
  loop limit-cycles 0.3–0.4 °C below SP, the learner goes DEGRADED, and PV stays under `SP − 0.2` while ≥ 900 s of ON-time
  accumulates without a 0.2 °C rise from the window start. E115 is a protection; its threshold is untouched. The fix belongs
  in the controller (Phase 3: keep PV inside the hysteresis band despite sensor lag), not in the watchdog.
* **S5 (note) a negative calibration offset never lowers the safety value** (verified through the production
  `max(raw, raw+offset)`): offset −0.5 gives peak ≤ 38.07 with no High on any plant.

## Not covered (stated, not claimed)
Sensor noise bursts, partial-frame corruption, RS485 stale-frame freshness timer (`dataValid` is modelled as on/off),
RTC/watchdog/task-starvation, relay-wear fault (RelayRateExceeded 305) during S2, real SSR failure statistics. Timing of the
5 ms control task is a host figure, not an on-chip one.
