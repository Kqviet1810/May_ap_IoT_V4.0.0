# Smart Thermal — Phase 6: incubation thermal programme and periodic egg cooling (design + host-tested modules, NOT in the firmware)

Files: `MAYAP_INDUSTRIAL_v1_0_0/thermal_program.h`, `egg_cooling.h` (new, **included by nothing**, so zero Flash/RAM/CPU and no change to any
control path), `tests/thermal-program.cpp` (440 648 checks incl. a 400 000-step fuzz, ASan+UBSan clean, in `tools/test_thermal_control.py`).
Both features are OFF by default; no species and no day schedule is shipped (that is a biological decision for the approved procedure, not a guess).

## 1. Three kinds of cycle, kept apart
| | owner | status |
|---|---|---|
| SSR burst window (one 16 kW bank, pulse density, arbiter) | `HeaterBurstScheduler` + `OutputArbiter` | **untouched**; never becomes a fixed 5 min ON / 5 min OFF |
| Thermal balance supervision (trend, drift, re-learn) | `ThermalLearner` (80 s windows, 22 min / 5.5 min memories, 90 min mismatch expiry) + Phase 3 hold convergence | runs from the 2 s sample path, never blocks the 5 ms control task |
| Batch / stage cycle (set point by incubation day, egg cooling) | this phase | design + pure modules |

## 2. `SetpointProgram` (per-day set point)
Table of ≤ 8 stages `{fromDay, target 0.1 °C}`; `validate()` rejects: empty, first day ≠ 1, non-increasing days, > 8 stages, any target outside
`[30.0, min(38.0, High − 0.7)]` °C, a step between consecutive stages > 0.5 °C, non-finite limits. The applied set point follows the stage target at
**≤ 0.5 °C/h**, a stalled task cannot become a jump (dt capped at 600 s), NaN/Inf and "no batch" return the configured set point untouched, an invalid
or empty programme is not applied at all. Stage changes are reported once through `takeEvent()` for the event log. Tests: a 0.4 °C stage change takes
47–49 min, the set point never leaves the band between the two targets, millis() rollover, stall, NaN, day 0.
Because the High/Emergency thresholds stay untouched and the safety temperature does not depend on the set point, a programme cannot move a protection.

## 3. `EggCooling` (periodic, independent, default OFF)
A *request* module: it returns `{heaterInhibit, lowTempSuppress}` and never touches an output. `heaterInhibit` can only REMOVE heat (the caller ANDs it
into the heater permit); `lowTempSuppress` may defer only the "temperature low" WARNING, never High/Emergency or a sensor/E115 fault.
* Policy (validated, else replaced by OFF): days 1–28, slot period ≤ 24 h and > the maximum duration, duration ≤ maximum ≤ **90 min** (absolute),
  **minimum temperature ≥ 30.0 °C** (absolute), resume policy.
* Schedule by RTC: slot k starts at `batchStart + k·period`; cooling is due in the first `duration` minutes of each slot of the allowed days.
* Preconditions rechecked on every call: batch running, RTC valid, sensor valid and finite, no safety latch, no AutoTune, no test mode. Losing any
  of them ends the cycle that instant (heater permitted again) and **consumes the slot** (no restart in the same slot). Also ends on
  minimum-temperature, maximum-duration, RTC moving backwards, or window end. Cooling does not start if the chamber is already within 0.2 °C of the minimum.
* Reset / power loss: **default CANCEL** — a window that is open at boot is not started from scratch. `ResumeRemaining` is opt-in and needs a CRC-valid
  record (magic, version, slot, slot start, cooling start), a valid RTC, the same slot, and the approved window still open; the maximum-duration guard
  keeps counting from the original cooling start. A torn/corrupt record is rejected by CRC → cancel.
* Fuzz invariants (400 000 random steps with random faults, resets and record round-trips): `heaterInhibit` ⇒ phase Cooling ∧ every precondition ∧
  temperature above the minimum ∧ cooling time below the maximum; `lowTempSuppress` ⇔ `heaterInhibit`; the fuzz really cools (> 2000 steps) and exercises
  MinTemp / Rtc / Sensor / Safety / Done exits.
This feature is not used to improve any PASS number, and it is not a stability feature.

## 4. What integration would need (not done; needs approval)
1. `normalSsrPermit &= !coolingOut.heaterInhibit` in `updateHeatingAndOutputs` (one place, AND-only, after every safety term);
   the existing low-temperature timer reads `coolingOut.lowTempSuppress`; the PID is reset when a cooling cycle ends (bumpless restart from zero).
2. A persisted `CoolingRecord` (20 B) via the existing A/B + CRC store, written on phase change from `loop()` only (endurance: ≤ 2 writes per cycle).
3. HMI/Web/protocol schema, validation, backward-compatible defaults (OFF) and the event codes (start, end + `CancelReason`).
4. Commissioning: the cooling of the *eggs* (not only the air) must be measured with reference probes before any production setting is chosen.
5. Set-point programme: schema + the per-species approved table; the HMI shows the stage and the applied (slewed) set point.

## 5. Not done / NOT TESTED
Anything in a real batch; the interaction with the vent schedule and with Smart AutoTune; flash-wear numbers; the HMI/Web surfaces.
