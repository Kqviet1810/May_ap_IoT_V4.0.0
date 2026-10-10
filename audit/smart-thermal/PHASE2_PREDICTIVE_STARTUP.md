# Smart Thermal — Phase 2: predictive startup (heat-in-flight brake)

SIMULATION ONLY. Feature flag `MAYAP_SMART_STARTUP` (build flag, **default 0 = bit-identical legacy**). Rollback = the same flag.
No threshold, GPIO, polarity, arbiter, fault or fail-safe was touched. Files: `MAYAP_INDUSTRIAL_v1_0_0/thermal_control.h`
(`ThermalStartupController`: +`setSmart/smart/observedGain`, `observeGain`, `smartCoast`, 2 members), `tests/thermal-smart-startup.cpp`
(new unit/property test, in `tools/test_thermal_control.py`), `tests/thermal-adaptive-sim.h` + `tests/thermal-adaptive-v1.cpp`
(`Mode::Smart`, `*_smart` suites, `case` command), `tools/ab_smart_thermal.py` (A/B/C), CI step.

## Root cause being addressed (Phase 0, F2)
The legacy energy model assumes ≤ 0.12 °C per ON-second (35 % reserve over 16 kW on 180 kJ/°C). A stronger plant
(32 of the 33 High cases are effectiveness 2.0 on 180 kJ/°C, Kh = 0.133–0.178) is under-predicted by up to 1.6×, and
dead time (up to 120 s) hides the response, so the brake sees nothing until the heat is already in flight.

## Algorithm (adds braking only; never adds heat)
`expectedCoast = max(legacy slope/energy estimate, smart term)` and the smart term is scaled by `(1 − hint strength)` so a
qualified learned profile takes over.
1. **Observed gain** `khObs`: running maximum of `slope / duty` taken only when the last 20 s of ACTUAL SSR ON-time is ≥ 85 %,
   the slope is > 0.01 °C/s and ≥ 20 s after the first visible rise (slope lags → under-reads; falling duty → over-reads;
   cap 0.5 °C/s). Used only when `khObs·1.15` exceeds the legacy 0.12 °C/s, so for every plant the legacy model already covers
   there is **no change at all**. Heat in flight = ON-time inside the observed apparent delay (+12 s) minus hold energy, times `khObs·1.15`.
2. **Unobserved-start prior** (`PriorGain` 0.196 °C/s = 16 kW × effectiveness 2.0 / 180 kJ/°C + 10 %): only while no rise has been
   seen, only for the first 150 s after the first heat (anything older has surfaced in the envelope: dead time ≤ 120 s +
   lag ≤ 30 s), and only when the start error is ≥ 3.5 °C (below that the 1.63× legacy error cannot reach the High margin).
   It bounds the energy committed before the plant answers, and releases afterwards so there is no permanent stall.
Everything is `float`, no allocation, +8 B RAM (two members), O(10) loop iterations on the 2 s decision sample; Flash and CPU on the
chip are NOT MEASURED (no `arduino-cli` here; CI size gate will report).

## Tests
* `tests/thermal-smart-startup.cpp` (ASan+UBSan clean): flag semantics (survives `reset()`), gain estimate within 0.8–1.4× of the true
  0.178 on the m1824 plant and < 0.05 on a heavy plant, **property: on identical inputs the Smart ceiling ≤ the legacy ceiling at
  every decision (7 plants, > 5000 decisions) and predicted peak ≥ legacy**, prior bounded (< 150 s ON before the first rise, vs
  105 s legacy on a 120 s dead-time strong plant ending at 42 °C), no permanent stall, small-error equality with legacy, millis rollover, NaN.
* The whole production-derived suite `tools/test_thermal_control.py --sanitize` is unchanged (output identical to the Phase 0 log
  except the new line). 788/2160/vent/hwchange/fault modes A and B are re-run from current source and reproduce the frozen baseline byte for byte.

## A/B/C result (`phase2/ab-summary.md`, per case id in `phase2/ab-788.csv`, `ab-2160.csv`)
| | A legacy | B Adaptive V1 | C Smart |
|---|---|---|---|
| 788 PASS | 186 | **509** | 506 |
| 788 reachable PASS | 139/489 | 387 | 387 |
| 788 High / Emergency | 1 / 0 | 0 / 0 | 0 / 0 |
| 2160 PASS | 319 | **1242** | 1227 |
| 2160 reachable PASS | 304/1535 | 1090 | 1074 |
| 2160 High cases | 33 | 33 | **18** |
| 2160 Emergency cases | 17 | 17 | **3** |
| B PASS → C FAIL / B FAIL → C PASS (788; 2160) | | | 14 / 11 ; 52 / 37 |
| vent 63: High+Emergency / tail MAE mean | 0 / 0.376 | 0 / 0.102 | 0 / 0.102 (identical) |
| hardware change, faults(30) | | | identical to B, High = Emergency = 0 |
| sensor-path suite (60 cases) | | ceilings met | ceilings met (4 stuck_low numbers differ, same verdict) |

m1824 (the case asked for): Adaptive V1 overshoot 1.794 °C / Emergency → Smart 0.026 °C, PASS. m1836 (dead 120 s): 5.21 → 0.61 °C, no High.

## Verdict
* **Safety: improved** (High cases 33 → 18, Emergency 17 → 3 on the identical plants; nothing worse anywhere, sensor-path unchanged).
* **STANDARD PASS: not improved** (−3 on 788, −15 on 2160). The task's goal "increase PASS" is **not met by this phase**; the change is kept
  OFF by default and recorded as a safety-positive / PASS-negative step. Not rejected because the rule "reject if it adds High/Emergency or
  weakens a guard" does not apply (it does neither), but it is not promotable on its own.
* Cause of the PASS loss (traced on cold#58, cold#34): a ≈ 100 s shift of the first burst changes what the learner sees; the hold
  estimator then converges slowly (1 pp / 6 min toward a true 11 %), a hold-mismatch puts Adaptive V1 in DEGRADED with feed-forward 0
  for ~70 min and PV parks 0.1–1 °C under SP. That is a learner weakness (Phase 3), not a startup-brake error: the same plants pass in B only
  because their start happens to feed the learner better.
* Remaining 18 High cases: 10 are start error 2.5 °C at ambient 35 (Kh 0.178 × the 15 s minimum observable pulse already exceeds the margin: not
  avoidable without a sub-observable probe; physics-limited), 8 are strong plants with dead time 60–120 s (ambient 10–28): root cause not yet analysed, tracked for Phase 3.

## Not tested / open
Smart AutoTune with the flag ON (the validation path shares the controller): NOT TESTED. On-chip Flash/RAM/CPU: NOT MEASURED. Interaction of the
flag with the real EEPROM profile after reboot (hint strength > 0 immediately): covered only by the unit property, not by a power-loss run.
