# Smart Thermal — Phase 3: learner hold convergence (PID/Adaptive optimisation)

SIMULATION ONLY. Flag `MAYAP_SMART_THERMAL` (umbrella, **default 0 = bit-identical Adaptive V1**; also `ThermalLearner::setSmartHold` /
`AdaptiveV1::setSmartLearning` for A/B). Rollback = rebuild with 0. No threshold, GPIO, arbiter, fault or fail-safe was touched.
Files: `thermal_learner.h` (hold tracking, +12 B RAM: 3 bools, 1 byte, 1 float), `thermal_adaptive_v1.h` (forwarding setter),
`thermal_assist.h` (umbrella macro), `tests/thermal-adaptive-unit.cpp` (+8 checks, now 745), `tests/thermal-adaptive-v1.cpp` (`holdout`
suite, `case` command incl. `hwchange`), `tools/ab_smart_thermal.py` (+holdout), artifacts in `phase3/`.

## Root cause (evidence, not assumption)
Of the 445 REACHABLE failures of Adaptive V1 in the 2160 matrix (frozen baseline), **227 (51 %) end with a "plant changed" flag**
(172 of them DEGRADED) in runs in which the plant never changes. In the 788 matrix 47 of the 54 mismatch declarations on failing
reachable cases are the *hold-power* detector (7 gain-ratio). Traces of `cold#32`, `cold#200`, `cold#212` (`HOLDWIN` debug):
1. the hold window measures the equilibrium duty correctly (flat windows 30.6, 31.7, 31.9, 32.9 % for a true 31.2 %),
2. the profile follows it by **at most +1 pp per window** (`HoldMaxRisePct`, 4.5 min windows, windows counted only within ±0.6 °C of SP),
   so after the 3 forming windows it needs ~20 windows (90 min) to reach the truth,
3. in the meantime the detector compares the (still low) profile with the measurement: deviation > max(5 pp, 25 %) for 3 windows
   declares a plant change → confidence halved, DEGRADED, hold feed-forward 0 for up to 90 min, PV parks 0.1–1 °C under SP.
The learner was alarming on its own convergence lag.

## Change (only the hold path)
* **Fast track**: two consecutive *flat* windows that agree (|Δ| ≤ max(2 pp, 10 %)) and exceed the profile move the hold by
  min(3 pp, ½·gap) toward **92 % of the lower measurement** (a flat window is a measurement of the equilibrium duty; the profile never goes
  past it). The 1 pp noise guard stays for a single window and for any rise not backed by two agreeing windows; falls unchanged.
* **Converging is not changing**: until the profile has agreed with two flat windows once (`holdSettled`), a higher measurement while the
  profile is still short of it is not fed to the plant-change detector. After that, or after any declared mismatch / set-point edit, the
  detector is exactly the legacy one (fixed reference, 3 same-sign windows), so a real plant change is still declared:
  heater at 60 %: mismatch declared by both (unit test); heater at 150 %: declared by both.
Hold feed-forward authority itself (`≤ 0.95 × hold`, confidence ramps, overshoot guard) is untouched.

## A/B/C (C = Phase 2 startup + Phase 3 hold). `phase3/ab-summary.md`, per case id `phase3/ab-*.csv`
| | A legacy | B Adaptive V1 | C Smart |
|---|---|---|---|
| 788 PASS / reachable | 186 / 139 | 509 / 387 | **546 / 418** (of 489) |
| 2160 PASS / reachable | 319 / 304 | 1242 / 1090 | **1397 / 1232** (of 1535) |
| 2160 High / Emergency cases | 33 / 17 | 33 / 17 | **18 / 3** |
| **HOLDOUT 360** PASS / reachable | 45 / 45 | 254 / 224 | **282 / 250** (of 290) — never used while tuning |
| B PASS → C FAIL ; B FAIL → C PASS (788 ; 2160 ; holdout) | | | 9 ; 46 — 48 ; 203 — 8 ; 36 |
| reachable overshoot > 0.30 °C (788 ; 2160 ; holdout) | | 6 ; 12 ; 8 | **2 ; 6 ; 3** |
| worst reachable overshoot, 2160 | | 0.498 | 0.375 |
| reachable mean MAE, 2160 | | 0.175 | 0.123 |
| vent 63: High+Emergency / tail MAE / wind-up | 0 / 0.376 / 48.9 | 0 / 0.102 / 5.6 | 0 / **0.093** / 5.6 |
| hardware change (16): High+Emergency / tail MAE mean | 0 / 0.458 | 0 / 0.109 | 0 / **0.083** |
| faults (30) | 0 / 0.539 | 0 / 0.036 | 0 / 0.033 |
| sensor-path suite (60): FALSE_TRIP_NO_FAULT | | 4 | **1** (3 nuisance E115 trips under probe lag 30 s removed) |
| Smart AutoTune 540 (flag off) | | 231 accepted, ACCEPTED_BAD 0 | unchanged (rows identical) |
Reproducibility: A and B rows re-run from current source equal the frozen baselines (788, 2160, holdout) row for row; the existing
`test_thermal_adaptive.py --full` gate and `test_thermal_control.py --sanitize` (ASan+UBSan) pass unchanged.
The fast-track step (3 vs 6 pp) was chosen on 788/2160 and checked on the holdout: 3 pp is equal or better everywhere (sweep 6 pp: 544/1397/282).

## Known costs and open items
* **Plant-change detection is slower on one plant**: heavy_d30 + heater 100→60 %: declared at 5224 s vs 1392 s (tail MAE 0.434 vs 0.063); the
  other three plants are detected at the same time as B (536, 536, 1460 s). Cause: that profile had not yet "settled" before the change.
* 48 (2160) / 9 (788) / 8 (holdout) cases pass in B and fail in C (reasons in `ab-*.csv`, mostly STEADY_MAE / SETTLING_TIMEOUT on dead-time 30–120 s
  plants); they are not explained individually.
* One sensor-path drift case moved from SAFE (peak 38.15) to UNDETECTED_TRUE_VIOLATION (peak just over 38.2).
* Remaining 18 High cases (10 start-error 2.5 °C at ambient 35 °C, physics-limited; 8 strong plants, dead time 60–120 s) and 3 Emergency: see Phase 2.
* NOT TESTED: 12 h / 24 h / 72 h runs of the Smart mode, Smart AutoTune with the flag ON, any on-chip measurement.
