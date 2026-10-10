# Smart Thermal — Phase 0: baseline & architecture audit

Branch `claude/gracious-hopper-zjvufg`, based on `8d51432` (87 commits ahead of `origin/main` `313a7ac`, none behind).
Working tree clean at start. Nothing here touches `main`, deploys, flashes hardware or changes any threshold.
Host: Linux x86-64, g++ 13.3, 4 cores. **`arduino-cli` is not installed in this container**, so the ESP32-S3
firmware build, flash/RAM figures and on-target timing are **NOT TESTED** in this phase (see "Open items").

## 1. Baseline reproduced (immutable artifacts in `baseline/`)

All numbers below were produced in this session by the repository's own gates, on this HEAD, before any edit.
They match the WSL2 report figures given in the task.

| Suite | Command | Result here | Task baseline |
|---|---|---|---|
| Adaptive 788 (STANDARD) | `tools/test_thermal_adaptive.py --full` | **509/788** (legacy V4 mode 186/788), High 0, Emergency 0 | 509/788 |
| Adaptive 788 reachable | same | 387/489 | – |
| Adaptive 2160 (STANDARD) | same | **1242/2160** (legacy V4 mode 319/2160), High **33**, Emergency **17** | 1242/2160, 33, 17 |
| Adaptive 2160 reachable | same | 1090/1535 | – |
| Ventilation 63 × 3 | same | High = Emergency = 0 (all three modes) | – |
| Hardware change / faults(30) | same | High = Emergency = 0 | – |
| Smart AutoTune 540 | `tools/test_smart_autotune.py` | **231/540** accepted, rejected 309 = POWER_LIMITED 142 + RELAY_FAILED 99 + VALIDATION_FAILED 57 + MODEL_INVALID 11, **ACCEPTED_BAD 0**, High/Emergency 0/0 | 231, 0 |
| Production-derived control suite | `tools/test_thermal_control.py` | all unit/regression gates PASS (adaptive-unit 737 checks, smart-autotune-unit 471 checks, arbiter/E115/EEPROM/filter) | – |

Not reproducible from the workspace: "Normal subset 788 45/47", "Normal subset 2160 187/213", "Extended normal 86/96",
and the "PRACTICAL" scoring (594/788, 1464/2160). Their subset definitions/oracle live in the WSL2 report, which was not
supplied. They are **NOT TESTED** here; no number is invented for them.

`baseline/MANIFEST.sha256` + `baseline/COMMANDS.md` give the files, SHA-256 and exact commands. Per-case CSVs
(`adaptive-788.csv`, `adaptive-2160.csv`, `smart-autotune-full540.csv`, ...) are the immutable per-case-ID reference for every later A/B.
Oracle (`r.pass`: overshoot ≤ 0.30, MAE ≤ 0.10, P95 ≤ 0.15, ripple ≤ 0.25, settled, High = Emergency = 0), SP, High 38.2 and
Emergency 39.0 are **unchanged**.

## 2. Verified control data-flow (read from `machine_control.h`, not assumed)

```
SHT30/RS485 frame ─ processSensor() ─ rawTemperature_ (offset only ever raises the safety value)
                                    └ filter (median-3 / IIR) ─ temperature_
safetyTemp = max(rawTemperature_, temperature_)  ──► updateAlarms(): Emergency (immediate), High (1 s confirm,
                                                     clear: -0.2 C / 10 s ; Emergency clear -0.3 C / 30 s) ──► faults_ (inhibit/drop)
temperature_ ─ ThermalObserver / ThermalLearner ─ AdaptiveV1 plan (Assist: hold FF, vent FF, Ki ceiling, limiter, StartupHint)
temperature_ ─ ThermalStartupController.decide()  → ceiling (predictive brake), freezePositiveIntegral
             ─ ThermalController (PID, beta=1, anti-windup, FF bumpless) → pidPower_
commandedPower = min(pidPower_, effectiveLimit)  [effectiveLimit = AdaptiveThermalSupervisor]
   ─ HeaterBurstScheduler (one 16 kW bank, pulse density) ─ req.heaterSsr
   ─ OutputArbiter.update(req) ─ GPIO1 (both SSRs) / master contactor
```
(`updateHeatingAndOutputs`, `machine_control.h:6473-6769`.) Only the OutputArbiter writes the heater GPIO. The PID, the
learner, AutoTune and the startup controller each *propose* a number; the final number is `min(...)` of the
proposals followed by permit gating (`normalSsrPermit`, `masterDropRequired`, `ssrInhibited`, `highTemperatureActive_`,
`emergencyActive_`, `sensorUsable_`, `abnormalResetLatched_`, storage faults). MQTT/Wi-Fi/Web are not in this path.

Existing state machines (so no second FSM is created): `ThermalStartupController::Phase {FullHeat, Approach,
SoftLanding, Hold}` ≈ WARMUP/APPROACH/BALANCE; `AdaptiveV1` learn states {UNLEARNED, LEARNING, QUALIFIED, ADAPTING,
DEGRADED}; `VentCoordinator {IDLE, PREPARE, ACTIVE, RECOVERY}` ≈ DISTURBANCE; safety latches ≈ FAULT_BYPASS.

## 3. Findings

### F1 (simulator integrity, P1 for the safety claim) — High/Emergency in the matrices come from plant truth
`tests/thermal-adaptive-sim.h` sets `highTemperatureActive_ = temp >= 38.2`, `emergencyActive_ = temp >= 39.0` and
`faults_.inhibit/drop` from the **true plant temperature**, instantaneously; it also feeds the true plant temperature as the
"raw" sample. Production derives them from `max(raw, filtered)` of the *reported* sensor with a 1 s High confirmation, and
the sensor has no lag in the model. Consequences: the matrices report whether the *plant* crossed a threshold, but the
inhibit the controller sees is idealised (zero detection latency, no quantisation/sensor-lag). The High/Emergency columns are
therefore a **plant-truth** metric, not evidence that the firmware's own safety path trips in time. Addressed in Phase 1.

### F2 (root cause of the 33 High / 17 Emergency) — startup energy model is blind to heaters stronger than its reserve
Every one of the 33 High cases is `capacity = 180 000 J/°C` (lightest class) with `eff` ≥ 1.5 (32 of 33 at `eff` = 2.0, one at 1.5),
i.e. true heating gain `Kh = 16000·eff/C` = 0.133–0.178 °C/s at 100 %. The startup controller's energy-in-flight estimate
(`ThermalStartupController::decide`) is `excessOnMs · 16 · 1.35 / 180000` = 0.12 °C per ON-second: a 35 % reserve over
a nominal 16 kW heater on the lightest assumed mass. Plants beyond that reserve (`Kh` > 0.12 °C/s) are under-predicted
by up to 1.5×. The learned `StartupHint` could correct it, but confidence/hint strength is zero until the learner
qualifies (≈ 1500 s in `m1824`), by which time the first overshoots have occurred. Nothing in the legacy path *measures*
the heater strength during the first burst and keeps it.

Trace of `m1824#1824` (eff 2.0, 180 kJ/K, loss 120 W/K, dead 60 s, lag 3 s, ambient 10 °C, SP 37.5):
1. 12 s–148 s heater at 100 %; PV first moves at t≈76 s (dead time 60 s + 3 s lag + filter). The brake works: heater is
   cut at t≈148 s at PV 22 °C with predicted peak 37.4 → the first coast tops out at 32.6 °C (no overshoot yet).
2. t=148…244 s heater is OFF; ≥ `coastSeconds` has elapsed and slope ≈ 0 so `coastCleared_` empties the ON-energy ring
   ("nothing in flight").
3. t=244 s: error 5.1 °C, slope ≈ 0, ring empty → `cap` rises 30 → 90 → 100 % (FullHeat). The estimate is
   `0.12 °C/s · ON-time` but the truth is 0.178 °C/s, and nothing is visible for the next 60 s dead time. About
   10 °C of heat is committed before the first sample can react; the brake only sees it later. Result: overshoot 1.79 °C,
   Emergency 1.
Baseline legacy mode has the identical overshoot (1.794), so it is not an Adaptive-V1 regression; V1 simply cannot help
before it has qualified.

So the heat is *not* "still rising after the command went to ~0 by magic": the command went to 100 % again on a wrong
energy model, and the delay line then delivered it. The first coast was handled correctly.

### F3 — other FAIL classes (counted from `baseline/adaptive-2160.csv`, V1 mode)
REACHABLE FAIL 445, SAFETY_LIMITED FAIL 438, HEAT_LIMITED FAIL 35 (physical heater-power limit, 5 duplicate-factor families). 3 cases pass on legacy and
fail on V1 in the 2160 matrix, 1 in the 788 matrix. A per-case reason label (task §17) is produced from the CSVs in the
A/B tool of a later phase; no label is asserted in this phase.

## 4. Risks / constraints recorded
* P1: F1 above (harness optimism). No P0 found in the control path.
* The frozen fixtures (`tests/fixtures/thermal-v1`, `thermal-v3-baseline`) are SHA-pinned by `test_thermal_control.py`; they must not be edited.
* Flash budget in CI: soft ceiling 1 480 000 B, static RAM 164 000 B (last measured: Flash 1 474 669, headroom ≈ 5 kB; RAM headroom < 1 kB per the V1 doc). Any new state must be a few dozen bytes or it breaks the RAM gate.
* `AdaptiveV1` costs 1688 B RAM; no PSRAM assumed.
* Host x86 timing is not evidence of the 5 ms on-target deadline.
